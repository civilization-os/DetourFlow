#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <detours.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <vector>
#include <iostream>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

// -------------------------------------------------------------------------
// Global state and thread-safety
// -------------------------------------------------------------------------
std::mutex g_Mutex;

struct ConnectTarget {
    bool isDomain = false;
    std::string domain;
    sockaddr_storage addr;
};

struct ConnectExContext {
    SOCKET s;
    ConnectTarget target;
    char* sendBuffer = nullptr;
    DWORD sendBufferLen = 0;
};

// Sockets that need handshaking on their first send (for connect/WSAConnect)
std::unordered_map<SOCKET, ConnectTarget> g_RedirectedSockets;

// Sockets tracking non-blocking state
std::unordered_map<SOCKET, bool> g_SocketNonBlockingState;

// Pending ConnectEx operations mapped by their overlapped pointers
std::unordered_map<LPOVERLAPPED, ConnectExContext> g_PendingConnectEx;

// -------------------------------------------------------------------------
// Fake IP DNS Mapping State
// -------------------------------------------------------------------------
uint32_t g_NextFakeIp = 0xC6120001; // 198.18.0.1
std::unordered_map<uint32_t, std::string> g_FakeIpToDomain;
std::unordered_set<PADDRINFOW> g_MyAllocatedAddrInfo;

// -------------------------------------------------------------------------
// Thread-safe Logging (Per-Process Log File)
// -------------------------------------------------------------------------
void Log(const char* format, ...) {
    char buffer[8192];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    buffer[sizeof(buffer) - 1] = '\0';
    va_end(args);

    OutputDebugStringA(buffer);

    // Append to process-specific log file in the same directory as the DLL to avoid cluttering app folders and hardcoding paths
    char filename[MAX_PATH] = { 0 };
    wchar_t dllPathW[MAX_PATH];
    if (GetModuleFileNameW(GetModuleHandleA("DetourFlow.dll"), dllPathW, MAX_PATH) > 0) {
        std::wstring dllDir = dllPathW;
        size_t lastSlash = dllDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            dllDir = dllDir.substr(0, lastSlash);
        }
        char dllDirA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, dllDir.c_str(), -1, dllDirA, MAX_PATH, NULL, NULL);
        sprintf_s(filename, "%s\\detour_flow_%u.log", dllDirA, GetCurrentProcessId());
    } else {
        sprintf_s(filename, "detour_flow_%u.log", GetCurrentProcessId());
    }

    FILE* f = nullptr;
    if (fopen_s(&f, filename, "a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buffer);
        fclose(f);
    }
}

// -------------------------------------------------------------------------
// Proxy configuration
// -------------------------------------------------------------------------
sockaddr_in GetProxyAddress4() {
    sockaddr_in proxyAddr = { 0 };
    proxyAddr.sin_family = AF_INET;
    
    std::string host = "127.0.0.1";
    int port = 7897;

    char envHost[256];
    size_t envHostLen = 0;
    if (getenv_s(&envHostLen, envHost, sizeof(envHost), "DETOUR_PROXY_HOST") == 0 && envHostLen > 0) {
        host = envHost;
    }

    char envPort[32];
    size_t envPortLen = 0;
    if (getenv_s(&envPortLen, envPort, sizeof(envPort), "DETOUR_PROXY_PORT") == 0 && envPortLen > 0) {
        port = std::stoi(envPort);
    }

    inet_pton(AF_INET, host.c_str(), &proxyAddr.sin_addr);
    proxyAddr.sin_port = htons(port);
    return proxyAddr;
}

sockaddr_in6 GetProxyAddress6() {
    sockaddr_in6 proxyAddr = { 0 };
    proxyAddr.sin6_family = AF_INET6;
    
    std::string host = "::ffff:127.0.0.1"; // IPv4-mapped IPv6 address for local proxy
    int port = 7897;

    char envHost[256];
    size_t envHostLen = 0;
    if (getenv_s(&envHostLen, envHost, sizeof(envHost), "DETOUR_PROXY_HOST") == 0 && envHostLen > 0) {
        // If an IPv4 proxy is provided in env, map it
        host = "::ffff:" + std::string(envHost);
    }

    char envPort[32];
    size_t envPortLen = 0;
    if (getenv_s(&envPortLen, envPort, sizeof(envPort), "DETOUR_PROXY_PORT") == 0 && envPortLen > 0) {
        port = std::stoi(envPort);
    }

    inet_pton(AF_INET6, host.c_str(), &proxyAddr.sin6_addr);
    proxyAddr.sin6_port = htons(port);
    return proxyAddr;
}

// -------------------------------------------------------------------------
// Address filtering & Fake IP resolution
// -------------------------------------------------------------------------
bool IsLocalIPv6(const in6_addr& addr) {
    // Check for loopback ::1
    bool isLoopback = true;
    for (int i = 0; i < 15; i++) {
        if (addr.s6_addr[i] != 0) {
            isLoopback = false;
            break;
        }
    }
    if (isLoopback && addr.s6_addr[15] == 1) {
        return true;
    }

    // Check for link-local fe80::/10
    if (addr.s6_addr[0] == 0xfe && (addr.s6_addr[1] & 0xc0) == 0x80) {
        return true;
    }

    // Check for IPv4-mapped IPv6 loopback (::ffff:127.x.x.x)
    if (addr.s6_addr[0] == 0 && addr.s6_addr[1] == 0 && addr.s6_addr[2] == 0 && addr.s6_addr[3] == 0 &&
        addr.s6_addr[4] == 0 && addr.s6_addr[5] == 0 && addr.s6_addr[6] == 0 && addr.s6_addr[7] == 0 &&
        addr.s6_addr[8] == 0 && addr.s6_addr[9] == 0 && addr.s6_addr[10] == 0xff && addr.s6_addr[11] == 0xff) {
        // Mapped IPv4
        uint32_t ip = (addr.s6_addr[12] << 24) | (addr.s6_addr[13] << 16) | (addr.s6_addr[14] << 8) | addr.s6_addr[15];
        if ((ip & 0xFF000000) == 0x7F000000) { // 127.0.0.0/8
            return true;
        }
    }

    return false;
}

bool ParseTargetAddress(const sockaddr* name, ConnectTarget& target) {
    if (!name) return false;

    target.isDomain = false;
    memcpy(&target.addr, name, sizeof(sockaddr_storage));

    if (name->sa_family == AF_INET) {
        const sockaddr_in* addr = (const sockaddr_in*)name;
        uint32_t ip = ntohl(addr->sin_addr.s_addr);
        USHORT port = ntohs(addr->sin_port);
        
        // Check if it is a Fake IP (198.18.0.0/15)
        if ((ip & 0xFFFE0000) == 0xC6120000) {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_FakeIpToDomain.find(ip);
            if (it != g_FakeIpToDomain.end()) {
                target.isDomain = true;
                target.domain = it->second;
                Log("Resolved Fake IP %d.%d.%d.%d:%u to Domain %s", 
                    (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port, target.domain.c_str());
                return true; 
            }
        }

        // Loopback: 127.0.0.0/8
        if ((ip & 0xFF000000) == 0x7F000000) {
            Log("DIRECT Loopback connect to %u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
            return false;
        }
        // Any: 0.0.0.0
        if (ip == 0) {
            Log("DIRECT Any connect to 0.0.0.0:%u", port);
            return false;
        }
        // Private IP ranges
        if ((ip & 0xFF000000) == 0x0A000000 || // 10.0.0.0/8
            (ip & 0xFFF00000) == 0xAC100000 || // 172.16.0.0/12
            (ip & 0xFFFF0000) == 0xC0A80000 || // 192.168.0.0/16
            (ip & 0xFFFF0000) == 0xA9FE0000) {  // 169.254.0.0/16 (APIPA)
            Log("DIRECT Private Network connect to %u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
            return false;
        }

        // Check against custom IP bypass whitelist (check detour_bypass.txt dynamically to support hot-reloading)
        std::vector<uint32_t> bypassIps;
        bool loadedFromFile = false;

        // Try reading detour_bypass.txt from DLL directory
        char dllPath[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(GetModuleHandleA("DetourFlow.dll"), dllPath, MAX_PATH) > 0) {
            std::string dllDir = dllPath;
            size_t lastSlash = dllDir.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                dllDir = dllDir.substr(0, lastSlash);
            }
            std::string bypassPath = dllDir + "\\detour_bypass.txt";
            FILE* f = nullptr;
            if (fopen_s(&f, bypassPath.c_str(), "r") == 0 && f) {
                char fileContent[4096] = { 0 };
                size_t readBytes = fread(fileContent, 1, sizeof(fileContent) - 1, f);
                fclose(f);
                if (readBytes > 0) {
                    std::string s(fileContent);
                    size_t start = 0;
                    size_t end = s.find(',');
                    while (end != std::string::npos) {
                        std::string ipStr = s.substr(start, end - start);
                        ipStr.erase(0, ipStr.find_first_not_of(" \t\r\n"));
                        ipStr.erase(ipStr.find_last_not_of(" \t\r\n") + 1);
                        if (!ipStr.empty()) {
                            IN_ADDR addr;
                            if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
                                bypassIps.push_back(ntohl(addr.s_addr));
                            }
                        }
                        start = end + 1;
                        end = s.find(',', start);
                    }
                    std::string ipStr = s.substr(start);
                    ipStr.erase(0, ipStr.find_first_not_of(" \t\r\n"));
                    ipStr.erase(ipStr.find_last_not_of(" \t\r\n") + 1);
                    if (!ipStr.empty()) {
                        IN_ADDR addr;
                        if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
                            bypassIps.push_back(ntohl(addr.s_addr));
                        }
                    }
                    loadedFromFile = true;
                }
            }
        }

        // Fallback to environment variable if file load failed or empty
        if (!loadedFromFile) {
            char envBypass[4096];
            size_t envBypassLen = 0;
            if (getenv_s(&envBypassLen, envBypass, sizeof(envBypass), "DETOUR_BYPASS_IPS") == 0 && envBypassLen > 0) {
                std::string s(envBypass);
                size_t start = 0;
                size_t end = s.find(',');
                while (end != std::string::npos) {
                    std::string ipStr = s.substr(start, end - start);
                    ipStr.erase(0, ipStr.find_first_not_of(" \t\r\n"));
                    ipStr.erase(ipStr.find_last_not_of(" \t\r\n") + 1);
                    if (!ipStr.empty()) {
                        IN_ADDR addr;
                        if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
                            bypassIps.push_back(ntohl(addr.s_addr));
                        }
                    }
                    start = end + 1;
                    end = s.find(',', start);
                }
                std::string ipStr = s.substr(start);
                ipStr.erase(0, ipStr.find_first_not_of(" \t\r\n"));
                ipStr.erase(ipStr.find_last_not_of(" \t\r\n") + 1);
                if (!ipStr.empty()) {
                    IN_ADDR addr;
                    if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
                        bypassIps.push_back(ntohl(addr.s_addr));
                    }
                }
            }
        }

        for (uint32_t bypassIp : bypassIps) {
            if (ip == bypassIp) {
                Log("DIRECT Whitelist Bypass connect to %u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
                return false;
            }
        }

        return true; 
    } else if (name->sa_family == AF_INET6) {
        // Direct route (bypass proxy) for raw IPv6 destinations.
        // Domain names resolved via GetAddrInfoW are already mapped to IPv4 Fake IPs (198.18.0.x),
        // which will successfully be proxied through AF_INET connects.
        // Raw IPv6 bypass prevents SOCKS5 handshake hangs on IPv6-only tests.
        const sockaddr_in6* addr = (const sockaddr_in6*)name;
        char ipStr[64] = {0};
        inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
        Log("DIRECT IPv6 connect to [%s]:%u", ipStr, ntohs(addr->sin6_port));
        return false;
    }
    return false; 
}

// -------------------------------------------------------------------------
// Original Winsock Function Pointers
// -------------------------------------------------------------------------
typedef int (WSAAPI *PFN_connect)(SOCKET s, const sockaddr *name, int namelen);
typedef int (WSAAPI *PFN_WSAConnect)(SOCKET s, const sockaddr *name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS);
typedef int (WSAAPI *PFN_closesocket)(SOCKET s);
typedef int (WSAAPI *PFN_ioctlsocket)(SOCKET s, long cmd, u_long *argp);
typedef int (WSAAPI *PFN_WSAEventSelect)(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents);
typedef int (WSAAPI *PFN_WSAIoctl)(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef INT (WSAAPI *PFN_GetAddrInfoW)(PCWSTR pNodeName, PCWSTR pServiceName, const ADDRINFOW *pHints, PADDRINFOW *ppResult);
typedef VOID (WSAAPI *PFN_FreeAddrInfoW)(PADDRINFOW pAddrInfo);

PFN_connect TrueConnect = connect;
PFN_WSAConnect TrueWSAConnect = WSAConnect;
PFN_closesocket TrueClosesocket = closesocket;
PFN_ioctlsocket TrueIoctlsocket = ioctlsocket;
PFN_WSAEventSelect TrueWSAEventSelect = WSAEventSelect;
PFN_WSAIoctl TrueWSAIoctl = WSAIoctl;
PFN_GetAddrInfoW TrueGetAddrInfoW = GetAddrInfoW;
PFN_FreeAddrInfoW TrueFreeAddrInfoW = FreeAddrInfoW;

// ConnectEx is dynamic
LPFN_CONNECTEX TrueConnectEx = nullptr;

// -------------------------------------------------------------------------
// Original Kernel32 Function Pointers
// -------------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_GetQueuedCompletionStatus)(HANDLE CompletionPort, LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey, LPOVERLAPPED *lpOverlapped, DWORD dwMilliseconds);
typedef BOOL (WINAPI *PFN_GetQueuedCompletionStatusEx)(HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount, PULONG lpNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable);
typedef HANDLE (WINAPI *PFN_CreateIoCompletionPort)(HANDLE FileHandle, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads);
typedef BOOL (WINAPI *PFN_CreateProcessA)(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
typedef BOOL (WINAPI *PFN_CreateProcessW)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

PFN_GetQueuedCompletionStatus TrueGetQueuedCompletionStatus = GetQueuedCompletionStatus;
PFN_GetQueuedCompletionStatusEx TrueGetQueuedCompletionStatusEx = GetQueuedCompletionStatusEx;
PFN_CreateIoCompletionPort TrueCreateIoCompletionPort = CreateIoCompletionPort;
PFN_CreateProcessA TrueCreateProcessA = CreateProcessA;
PFN_CreateProcessW TrueCreateProcessW = CreateProcessW;

// -------------------------------------------------------------------------
// SOCKS5 Handshake Implementation
// -------------------------------------------------------------------------
bool PerformSocks5Handshake(SOCKET s, const ConnectTarget& target) {
    Log("Beginning SOCKS5 handshake for socket %u", (unsigned int)s);
    if (target.isDomain) {
        Log("Target is domain: %s", target.domain.c_str());
    } else {
        if (target.addr.ss_family == AF_INET) {
            sockaddr_in* addr = (sockaddr_in*)&target.addr;
            uint32_t ip = ntohl(addr->sin_addr.s_addr);
            Log("Target is IP: %u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, ntohs(addr->sin_port));
        } else if (target.addr.ss_family == AF_INET6) {
            char ipStr[64] = {0};
            sockaddr_in6* addr = (sockaddr_in6*)&target.addr;
            inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
            Log("Target is IP: [%s]:%u", ipStr, ntohs(addr->sin6_port));
        }
    }

    unsigned long nonBlockMode = 0; 
    if (TrueIoctlsocket(s, FIONBIO, &nonBlockMode) != 0) {
        Log("Failed to set socket %u to blocking mode", (unsigned int)s);
        return false;
    }

    bool success = false;
    do {
        unsigned char greeting[] = { 0x05, 0x01, 0x00 };
        if (send(s, (const char*)greeting, sizeof(greeting), 0) != sizeof(greeting)) {
            Log("Failed to send SOCKS5 greeting");
            break;
        }

        unsigned char greetingResp[2];
        int recved = recv(s, (char*)greetingResp, sizeof(greetingResp), MSG_WAITALL);
        if (recved != 2 || greetingResp[0] != 0x05 || greetingResp[1] != 0x00) {
            Log("SOCKS5 proxy rejected greeting: recved=%d", recved);
            break;
        }

        unsigned char request[512];
        int reqLen = 0;
        request[0] = 0x05;
        request[1] = 0x01;
        request[2] = 0x00;

        if (target.isDomain) {
            request[3] = 0x03;
            size_t len = target.domain.length();
            if (len > 255) len = 255;
            request[4] = (unsigned char)len;
            memcpy(&request[5], target.domain.c_str(), len);
            
            USHORT port = 0;
            if (target.addr.ss_family == AF_INET) {
                port = ((sockaddr_in*)&target.addr)->sin_port;
            } else if (target.addr.ss_family == AF_INET6) {
                port = ((sockaddr_in6*)&target.addr)->sin6_port;
            }
            memcpy(&request[5 + len], &port, 2);
            reqLen = 5 + (int)len + 2;
        } else {
            if (target.addr.ss_family == AF_INET) {
                request[3] = 0x01;
                const sockaddr_in* addr = (const sockaddr_in*)&target.addr;
                memcpy(&request[4], &addr->sin_addr.s_addr, 4);
                memcpy(&request[8], &addr->sin_port, 2);
                reqLen = 10;
            } else if (target.addr.ss_family == AF_INET6) {
                request[3] = 0x04;
                const sockaddr_in6* addr = (const sockaddr_in6*)&target.addr;
                memcpy(&request[4], &addr->sin6_addr.s6_addr, 16);
                memcpy(&request[20], &addr->sin6_port, 2);
                reqLen = 22;
            } else {
                Log("Unsupported address family %d", target.addr.ss_family);
                break;
            }
        }

        if (send(s, (const char*)request, reqLen, 0) != reqLen) {
            Log("Failed to send CONNECT request");
            break;
        }

        unsigned char respHeader[4];
        if (recv(s, (char*)respHeader, sizeof(respHeader), MSG_WAITALL) != 4) {
            Log("Failed to read CONNECT response header");
            break;
        }

        if (respHeader[0] != 0x05 || respHeader[1] != 0x00) {
            Log("SOCKS5 proxy returned error status: %02X", respHeader[1]);
            break;
        }

        int remainingLen = 0;
        if (respHeader[3] == 0x01) {
            remainingLen = 4 + 2;
        } else if (respHeader[3] == 0x03) {
            unsigned char domainLen = 0;
            if (recv(s, (char*)&domainLen, 1, 0) != 1) {
                break;
            }
            remainingLen = domainLen + 2;
        } else if (respHeader[3] == 0x04) {
            remainingLen = 16 + 2;
        } else {
            break;
        }

        if (remainingLen > 0) {
            std::vector<unsigned char> dummy(remainingLen);
            if (recv(s, (char*)dummy.data(), remainingLen, MSG_WAITALL) != remainingLen) {
                break;
            }
        }

        success = true;
    } while (false);

    bool isNonBlocking = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_SocketNonBlockingState.find(s);
        if (it != g_SocketNonBlockingState.end()) {
            isNonBlocking = it->second;
        }
    }

    if (isNonBlocking) {
        unsigned long restoreMode = 1;
        TrueIoctlsocket(s, FIONBIO, &restoreMode);
    }

    return success;
}

// -------------------------------------------------------------------------
// Helper for ConnectEx Completion Processing
// -------------------------------------------------------------------------
void CheckAndHandleConnectExCompletion(LPOVERLAPPED lpOverlapped) {
    ConnectExContext ctx = { 0 };
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_PendingConnectEx.find(lpOverlapped);
        if (it != g_PendingConnectEx.end()) {
            ctx = it->second;
            found = true;
            g_PendingConnectEx.erase(it);
        }
    }

    if (found) {
        std::string targetDesc = ctx.target.isDomain ? ctx.target.domain : "";
        if (!ctx.target.isDomain && ctx.target.addr.ss_family == AF_INET) {
            char ipStr[64] = {0};
            sockaddr_in* addr = (sockaddr_in*)&ctx.target.addr;
            inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
            targetDesc = std::string(ipStr) + ":" + std::to_string(ntohs(addr->sin_port));
        } else if (!ctx.target.isDomain && ctx.target.addr.ss_family == AF_INET6) {
            char ipStr[64] = {0};
            sockaddr_in6* addr = (sockaddr_in6*)&ctx.target.addr;
            inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
            targetDesc = "[" + std::string(ipStr) + "]:" + std::to_string(ntohs(addr->sin6_port));
        }

        Log("ConnectEx completed asynchronously. Socket %u context updated. Beginning handshake to target %s...", (unsigned int)ctx.s, targetDesc.c_str());
        // Update socket context so that send/recv can be used on it
        setsockopt(ctx.s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);

        PerformSocks5Handshake(ctx.s, ctx.target);
        if (ctx.sendBuffer && ctx.sendBufferLen > 0) {
            send(ctx.s, ctx.sendBuffer, ctx.sendBufferLen, 0);
            delete[] ctx.sendBuffer;
        }
    }
}

// -------------------------------------------------------------------------
// Hook Implementations (Winsock)
// -------------------------------------------------------------------------

int WSAAPI HookConnect(SOCKET s, const sockaddr *name, int namelen) {
    if (name && name->sa_family == AF_INET6) {
        const sockaddr_in6* addr = (const sockaddr_in6*)name;
        if (!IsLocalIPv6(addr->sin6_addr)) {
            char ipStr[64] = {0};
            inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
            Log("REJECTING external IPv6 connect to [%s]:%u to force IPv4 fallback", ipStr, ntohs(addr->sin6_port));
            WSASetLastError(WSAENETUNREACH);
            return SOCKET_ERROR;
        }
    }

    ConnectTarget target;
    if (!ParseTargetAddress(name, target)) {
        return TrueConnect(s, name, namelen);
    }

    std::string targetDesc = target.isDomain ? target.domain : "";
    if (!target.isDomain && target.addr.ss_family == AF_INET) {
        char ipStr[64] = {0};
        sockaddr_in* addr = (sockaddr_in*)&target.addr;
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
        targetDesc = std::string(ipStr) + ":" + std::to_string(ntohs(addr->sin_port));
    } else if (!target.isDomain && target.addr.ss_family == AF_INET6) {
        char ipStr[64] = {0};
        sockaddr_in6* addr = (sockaddr_in6*)&target.addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
        targetDesc = "[" + std::string(ipStr) + "]:" + std::to_string(ntohs(addr->sin6_port));
    }

    Log("Intercepted connect() on socket %u. Target: %s. Redirecting to proxy.", (unsigned int)s, targetDesc.c_str());
    
    // Check if the socket is non-blocking
    bool isNonBlocking = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_SocketNonBlockingState.find(s);
        if (it != g_SocketNonBlockingState.end()) {
            isNonBlocking = it->second;
        }
    }

    if (isNonBlocking) {
        unsigned long blockMode = 0;
        TrueIoctlsocket(s, FIONBIO, &blockMode);
    }

    // Connect to the SOCKS5 proxy synchronously, using the appropriate family
    int res = SOCKET_ERROR;
    if (target.addr.ss_family == AF_INET6) {
        sockaddr_in6 proxyAddr = GetProxyAddress6();
        res = TrueConnect(s, (const sockaddr*)&proxyAddr, sizeof(proxyAddr));
    } else {
        sockaddr_in proxyAddr = GetProxyAddress4();
        res = TrueConnect(s, (const sockaddr*)&proxyAddr, sizeof(proxyAddr));
    }

    if (res == SOCKET_ERROR) {
        DWORD err = WSAGetLastError();
        Log("TrueConnect to proxy failed for target %s. Error: %u", targetDesc.c_str(), err);
        if (isNonBlocking) {
            unsigned long restoreMode = 1;
            TrueIoctlsocket(s, FIONBIO, &restoreMode);
        }
        return SOCKET_ERROR;
    }

    // Perform SOCKS5 handshake synchronously
    if (!PerformSocks5Handshake(s, target)) {
        Log("SOCKS5 handshake failed on socket %u for target %s", (unsigned int)s, targetDesc.c_str());
        if (isNonBlocking) {
            unsigned long restoreMode = 1;
            TrueIoctlsocket(s, FIONBIO, &restoreMode);
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    // Restore non-blocking state if needed
    if (isNonBlocking) {
        unsigned long restoreMode = 1;
        TrueIoctlsocket(s, FIONBIO, &restoreMode);
    }

    Log("SOCKS5 connection & handshake successful for socket %u to target %s", (unsigned int)s, targetDesc.c_str());
    WSASetLastError(0);
    return 0;
}

int WSAAPI HookWSAConnect(SOCKET s, const sockaddr *name, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    if (name && name->sa_family == AF_INET6) {
        const sockaddr_in6* addr = (const sockaddr_in6*)name;
        if (!IsLocalIPv6(addr->sin6_addr)) {
            char ipStr[64] = {0};
            inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
            Log("REJECTING external IPv6 WSAConnect to [%s]:%u to force IPv4 fallback", ipStr, ntohs(addr->sin6_port));
            WSASetLastError(WSAENETUNREACH);
            return SOCKET_ERROR;
        }
    }

    ConnectTarget target;
    if (!ParseTargetAddress(name, target)) {
        return TrueWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    }

    std::string targetDesc = target.isDomain ? target.domain : "";
    if (!target.isDomain && target.addr.ss_family == AF_INET) {
        char ipStr[64] = {0};
        sockaddr_in* addr = (sockaddr_in*)&target.addr;
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
        targetDesc = std::string(ipStr) + ":" + std::to_string(ntohs(addr->sin_port));
    } else if (!target.isDomain && target.addr.ss_family == AF_INET6) {
        char ipStr[64] = {0};
        sockaddr_in6* addr = (sockaddr_in6*)&target.addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
        targetDesc = "[" + std::string(ipStr) + "]:" + std::to_string(ntohs(addr->sin6_port));
    }

    Log("Intercepted WSAConnect() on socket %u. Target: %s. Redirecting to proxy.", (unsigned int)s, targetDesc.c_str());
    
    bool isNonBlocking = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_SocketNonBlockingState.find(s);
        if (it != g_SocketNonBlockingState.end()) {
            isNonBlocking = it->second;
        }
    }

    if (isNonBlocking) {
        unsigned long blockMode = 0;
        TrueIoctlsocket(s, FIONBIO, &blockMode);
    }

    int res = SOCKET_ERROR;
    if (target.addr.ss_family == AF_INET6) {
        sockaddr_in6 proxyAddr = GetProxyAddress6();
        res = TrueWSAConnect(s, (const sockaddr*)&proxyAddr, sizeof(proxyAddr), lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    } else {
        sockaddr_in proxyAddr = GetProxyAddress4();
        res = TrueWSAConnect(s, (const sockaddr*)&proxyAddr, sizeof(proxyAddr), lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    }

    if (res == SOCKET_ERROR) {
        DWORD err = WSAGetLastError();
        Log("TrueWSAConnect to proxy failed for target %s. Error: %u", targetDesc.c_str(), err);
        if (isNonBlocking) {
            unsigned long restoreMode = 1;
            TrueIoctlsocket(s, FIONBIO, &restoreMode);
        }
        return SOCKET_ERROR;
    }

    if (!PerformSocks5Handshake(s, target)) {
        Log("SOCKS5 handshake failed on socket %u for target %s", (unsigned int)s, targetDesc.c_str());
        if (isNonBlocking) {
            unsigned long restoreMode = 1;
            TrueIoctlsocket(s, FIONBIO, &restoreMode);
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    if (isNonBlocking) {
        unsigned long restoreMode = 1;
        TrueIoctlsocket(s, FIONBIO, &restoreMode);
    }

    Log("SOCKS5 connection & handshake successful for socket %u to target %s", (unsigned int)s, targetDesc.c_str());
    WSASetLastError(0);
    return 0;
}

BOOL PASCAL FAR HookConnectEx(
    SOCKET s,
    const struct sockaddr FAR *name,
    int namelen,
    PVOID lpSendBuffer,
    DWORD dwSendDataLength,
    LPDWORD lpdwBytesSent,
    LPOVERLAPPED lpOverlapped
) {
    if (name && name->sa_family == AF_INET6) {
        const sockaddr_in6* addr = (const sockaddr_in6*)name;
        if (!IsLocalIPv6(addr->sin6_addr)) {
            char ipStr[64] = {0};
            inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
            Log("REJECTING external IPv6 ConnectEx to [%s]:%u to force IPv4 fallback", ipStr, ntohs(addr->sin6_port));
            WSASetLastError(WSAENETUNREACH);
            return FALSE;
        }
    }

    ConnectTarget target;
    if (!ParseTargetAddress(name, target)) {
        return TrueConnectEx(s, name, namelen, lpSendBuffer, dwSendDataLength, lpdwBytesSent, lpOverlapped);
    }

    std::string targetDesc = target.isDomain ? target.domain : "";
    if (!target.isDomain && target.addr.ss_family == AF_INET) {
        char ipStr[64] = {0};
        sockaddr_in* addr = (sockaddr_in*)&target.addr;
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
        targetDesc = std::string(ipStr) + ":" + std::to_string(ntohs(addr->sin_port));
    } else if (!target.isDomain && target.addr.ss_family == AF_INET6) {
        char ipStr[64] = {0};
        sockaddr_in6* addr = (sockaddr_in6*)&target.addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr));
        targetDesc = "[" + std::string(ipStr) + "]:" + std::to_string(ntohs(addr->sin6_port));
    }

    Log("Intercepted ConnectEx() on socket %u. Target: %s. Redirecting to proxy.", (unsigned int)s, targetDesc.c_str());
    ConnectExContext ctx;
    ctx.s = s;
    ctx.target = target;
    if (lpSendBuffer && dwSendDataLength > 0) {
        ctx.sendBuffer = new char[dwSendDataLength];
        memcpy(ctx.sendBuffer, lpSendBuffer, dwSendDataLength);
        ctx.sendBufferLen = dwSendDataLength;
    }

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingConnectEx[lpOverlapped] = ctx;
    }

    if (target.addr.ss_family == AF_INET6) {
        sockaddr_in6 proxyAddr = GetProxyAddress6();
        return TrueConnectEx(s, (const sockaddr*)&proxyAddr, sizeof(proxyAddr), NULL, 0, lpdwBytesSent, lpOverlapped);
    } else {
        sockaddr_in proxyAddr = GetProxyAddress4();
        return TrueConnectEx(s, (const sockaddr*)&proxyAddr, sizeof(proxyAddr), NULL, 0, lpdwBytesSent, lpOverlapped);
    }
}

int WSAAPI HookWSAIoctl(
    SOCKET s,
    DWORD dwIoControlCode,
    LPVOID lpvInBuffer,
    DWORD cbInBuffer,
    LPVOID lpvOutBuffer,
    DWORD cbOutBuffer,
    LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine
) {
    if (dwIoControlCode == SIO_GET_EXTENSION_FUNCTION_POINTER && cbInBuffer >= sizeof(GUID)) {
        GUID* guid = (GUID*)lpvInBuffer;
        if (IsEqualGUID(*guid, WSAID_CONNECTEX)) {
            int res = TrueWSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer, lpvOutBuffer, cbOutBuffer, lpcbBytesReturned, lpOverlapped, lpCompletionRoutine);
            if (res == 0 && lpvOutBuffer && cbOutBuffer >= sizeof(LPFN_CONNECTEX)) {
                Log("Captured ConnectEx pointer retrieval via WSAIoctl");
                TrueConnectEx = *(LPFN_CONNECTEX*)lpvOutBuffer;
                *(LPFN_CONNECTEX*)lpvOutBuffer = HookConnectEx;
            }
            return res;
        }
    }
    return TrueWSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer, lpvOutBuffer, cbOutBuffer, lpcbBytesReturned, lpOverlapped, lpCompletionRoutine);
}

int WSAAPI HookClosesocket(SOCKET s) {
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_RedirectedSockets.erase(s);
        g_SocketNonBlockingState.erase(s);
    }
    return TrueClosesocket(s);
}

int WSAAPI HookIoctlsocket(SOCKET s, long cmd, u_long *argp) {
    int res = TrueIoctlsocket(s, cmd, argp);
    if (res == 0 && cmd == FIONBIO && argp) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_SocketNonBlockingState[s] = (*argp != 0);
    }
    return res;
}

int WSAAPI HookWSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents) {
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_SocketNonBlockingState[s] = true;
    }
    return TrueWSAEventSelect(s, hEventObject, lNetworkEvents);
}

HANDLE WINAPI HookCreateIoCompletionPort(HANDLE FileHandle, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads) {
    int optVal = 0;
    int optLen = sizeof(optVal);
    if (getsockopt((SOCKET)FileHandle, SOL_SOCKET, SO_TYPE, (char*)&optVal, &optLen) == 0) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_SocketNonBlockingState[(SOCKET)FileHandle] = true;
    }
    return TrueCreateIoCompletionPort(FileHandle, ExistingCompletionPort, CompletionKey, NumberOfConcurrentThreads);
}


// -------------------------------------------------------------------------
// Fake IP DNS Hooks
// -------------------------------------------------------------------------
bool IsIPAddress(const wchar_t* str) {
    if (!str) return false;
    IN_ADDR ipv4;
    IN6_ADDR ipv6;
    if (InetPtonW(AF_INET, str, &ipv4) == 1) return true;
    if (InetPtonW(AF_INET6, str, &ipv6) == 1) return true;
    return false;
}

DWORD AllocateFakeIp(const wchar_t* domainW) {
    char domainA[256];
    WideCharToMultiByte(CP_UTF8, 0, domainW, -1, domainA, sizeof(domainA), NULL, NULL);

    std::lock_guard<std::mutex> lock(g_Mutex);
    DWORD fakeIp = g_NextFakeIp++;
    g_FakeIpToDomain[fakeIp] = domainA;
    return fakeIp;
}

INT WSAAPI HookGetAddrInfoW(
    PCWSTR pNodeName,
    PCWSTR pServiceName,
    const ADDRINFOW *pHints,
    PADDRINFOW *ppResult
) {
    if (pNodeName && !IsIPAddress(pNodeName) && _wcsicmp(pNodeName, L"localhost") != 0) {
        DWORD fakeIp = AllocateFakeIp(pNodeName);
        Log("Intercepted GetAddrInfoW for domain %ws. Allocating Fake IP %u.%u.%u.%u", 
            pNodeName, (fakeIp >> 24) & 0xFF, (fakeIp >> 16) & 0xFF, (fakeIp >> 8) & 0xFF, fakeIp & 0xFF);

        PADDRINFOW res = (PADDRINFOW)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ADDRINFOW) + sizeof(sockaddr_in));
        if (!res) return EAI_MEMORY;

        sockaddr_in* addr = (sockaddr_in*)((char*)res + sizeof(ADDRINFOW));
        
        res->ai_family = AF_INET;
        res->ai_socktype = pHints ? pHints->ai_socktype : SOCK_STREAM;
        res->ai_protocol = pHints ? pHints->ai_protocol : IPPROTO_TCP;
        res->ai_addrlen = sizeof(sockaddr_in);
        res->ai_addr = (sockaddr*)addr;
        
        addr->sin_family = AF_INET;
        addr->sin_addr.s_addr = htonl(fakeIp);
        if (pServiceName) {
            addr->sin_port = htons((USHORT)_wtoi(pServiceName));
        }

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_MyAllocatedAddrInfo.insert(res);
        }

        *ppResult = res;
        return 0;
    }
    return TrueGetAddrInfoW(pNodeName, pServiceName, pHints, ppResult);
}

VOID WSAAPI HookFreeAddrInfoW(PADDRINFOW pAddrInfo) {
    bool mine = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_MyAllocatedAddrInfo.find(pAddrInfo);
        if (it != g_MyAllocatedAddrInfo.end()) {
            mine = true;
            g_MyAllocatedAddrInfo.erase(it);
        }
    }
    if (mine) {
        HeapFree(GetProcessHeap(), 0, pAddrInfo);
    } else {
        TrueFreeAddrInfoW(pAddrInfo);
    }
}

// -------------------------------------------------------------------------
// Process Spawning Hooks (Ensures inheritance by child processes)
// -------------------------------------------------------------------------
BOOL WINAPI HookCreateProcessW(
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
) {
    std::wstring cmd = lpCommandLine ? lpCommandLine : L"NULL";
    if (cmd.length() > 256) {
        cmd = cmd.substr(0, 256) + L"...";
    }
    Log("Intercepted CreateProcessW. Target command: %ws", cmd.c_str());

    std::wstring cmdStr = lpCommandLine ? lpCommandLine : L"";
    std::wstring appStr = lpApplicationName ? lpApplicationName : L"";
    
    // Check if the spawned process is a build tool, compiler, or version control tool
    bool isBuildTool = false;
    const wchar_t* buildTools[] = {
        L"cargo.exe", L"rustc.exe", L"git.exe", L"cl.exe", L"link.exe", 
        L"msbuild.exe", L"cmake.exe", L"ninja.exe", L"tar.exe", L"powershell.exe"
    };
    for (const wchar_t* tool : buildTools) {
        if (cmdStr.find(tool) != std::wstring::npos || appStr.find(tool) != std::wstring::npos) {
            isBuildTool = true;
            break;
        }
    }

    if (isBuildTool) {
        Log("Bypassing DetourFlow.dll injection for build tool to prevent locks/network errors.");
        return TrueCreateProcessW(
            lpApplicationName,
            lpCommandLine,
            lpProcessAttributes,
            lpThreadAttributes,
            bInheritHandles,
            dwCreationFlags,
            lpEnvironment,
            lpCurrentDirectory,
            lpStartupInfo,
            lpProcessInformation
        );
    }

    std::wstring modifiedCmdLine;
    LPWSTR targetCmdLine = lpCommandLine;
    if (lpCommandLine) {
        bool isChrome = (cmdStr.find(L"chrome.exe") != std::wstring::npos || appStr.find(L"chrome.exe") != std::wstring::npos);
        if (isChrome && cmdStr.find(L"--no-sandbox") == std::wstring::npos) {
            modifiedCmdLine = cmdStr + L" --no-sandbox";
            targetCmdLine = &modifiedCmdLine[0];
            Log("Adding --no-sandbox to Chrome command line: %ws", targetCmdLine);
        }
    }

    wchar_t dllPathW[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleA("DetourFlow.dll"), dllPathW, MAX_PATH);

    char dllPathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, dllPathW, -1, dllPathA, MAX_PATH, NULL, NULL);

    BOOL res = DetourCreateProcessWithDllExW(
        lpApplicationName,
        targetCmdLine,
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        dwCreationFlags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation,
        dllPathA,
        TrueCreateProcessW
    );

    if (res) {
        Log("Successfully propagated DetourFlow.dll to spawned child process PID %u", lpProcessInformation->dwProcessId);
    } else {
        Log("Failed to propagate DLL to child process. Error: %u", GetLastError());
    }
    return res;
}

BOOL WINAPI HookCreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
) {
    std::string cmd = lpCommandLine ? lpCommandLine : "NULL";
    if (cmd.length() > 256) {
        cmd = cmd.substr(0, 256) + "...";
    }
    Log("Intercepted CreateProcessA. Target command: %s", cmd.c_str());

    std::string cmdStr = lpCommandLine ? lpCommandLine : "";
    std::string appStr = lpApplicationName ? lpApplicationName : "";

    // Check if the spawned process is a build tool, compiler, or version control tool
    bool isBuildTool = false;
    const char* buildTools[] = {
        "cargo.exe", "rustc.exe", "git.exe", "cl.exe", "link.exe", 
        "msbuild.exe", "cmake.exe", "ninja.exe", "tar.exe", "powershell.exe"
    };
    for (const char* tool : buildTools) {
        if (cmdStr.find(tool) != std::string::npos || appStr.find(tool) != std::string::npos) {
            isBuildTool = true;
            break;
        }
    }

    if (isBuildTool) {
        Log("Bypassing DetourFlow.dll injection for build tool (A) to prevent locks/network errors.");
        return TrueCreateProcessA(
            lpApplicationName,
            lpCommandLine,
            lpProcessAttributes,
            lpThreadAttributes,
            bInheritHandles,
            dwCreationFlags,
            lpEnvironment,
            lpCurrentDirectory,
            lpStartupInfo,
            lpProcessInformation
        );
    }

    std::string modifiedCmdLine;
    LPSTR targetCmdLine = lpCommandLine;
    if (lpCommandLine) {
        bool isChrome = (cmdStr.find("chrome.exe") != std::string::npos || appStr.find("chrome.exe") != std::string::npos);
        if (isChrome && cmdStr.find("--no-sandbox") == std::string::npos) {
            modifiedCmdLine = cmdStr + " --no-sandbox";
            targetCmdLine = &modifiedCmdLine[0];
            Log("Adding --no-sandbox to Chrome command line: %s", targetCmdLine);
        }
    }

    wchar_t dllPathW[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleA("DetourFlow.dll"), dllPathW, MAX_PATH);

    char dllPathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, dllPathW, -1, dllPathA, MAX_PATH, NULL, NULL);

    BOOL res = DetourCreateProcessWithDllExA(
        lpApplicationName,
        targetCmdLine,
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        dwCreationFlags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation,
        dllPathA,
        TrueCreateProcessA
    );

    if (res) {
        Log("Successfully propagated DetourFlow.dll to spawned child process PID %u", lpProcessInformation->dwProcessId);
    } else {
        Log("Failed to propagate DLL to child process. Error: %u", GetLastError());
    }
    return res;
}

// -------------------------------------------------------------------------
// Kernel32 IOCP Hooks
// -------------------------------------------------------------------------

BOOL WINAPI HookGetQueuedCompletionStatus(
    HANDLE CompletionPort,
    LPDWORD lpNumberOfBytesTransferred,
    PULONG_PTR lpCompletionKey,
    LPOVERLAPPED *lpOverlapped,
    DWORD dwMilliseconds
) {
    BOOL res = TrueGetQueuedCompletionStatus(CompletionPort, lpNumberOfBytesTransferred, lpCompletionKey, lpOverlapped, dwMilliseconds);
    if (res && lpOverlapped && *lpOverlapped) {
        CheckAndHandleConnectExCompletion(*lpOverlapped);
    }
    return res;
}

BOOL WINAPI HookGetQueuedCompletionStatusEx(
    HANDLE CompletionPort,
    LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    ULONG ulCount,
    PULONG lpNumEntriesRemoved,
    DWORD dwMilliseconds,
    BOOL fAlertable
) {
    BOOL res = TrueGetQueuedCompletionStatusEx(CompletionPort, lpCompletionPortEntries, ulCount, lpNumEntriesRemoved, dwMilliseconds, fAlertable);
    if (res && lpNumEntriesRemoved && lpCompletionPortEntries) {
        for (ULONG i = 0; i < *lpNumEntriesRemoved; i++) {
            if (lpCompletionPortEntries[i].lpOverlapped) {
                CheckAndHandleConnectExCompletion(lpCompletionPortEntries[i].lpOverlapped);
            }
        }
    }
    return res;
}

// -------------------------------------------------------------------------
// Detours Injection DLL Entry Point
// -------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        wchar_t exePathW[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, exePathW, MAX_PATH);
        std::wstring exeStr(exePathW);

        bool isBuildTool = false;
        const wchar_t* buildTools[] = {
            L"cargo.exe", L"rustc.exe", L"git.exe", L"cl.exe", L"link.exe", 
            L"msbuild.exe", L"cmake.exe", L"ninja.exe", L"tar.exe", L"powershell.exe"
        };
        for (const wchar_t* tool : buildTools) {
            if (exeStr.find(tool) != std::wstring::npos) {
                isBuildTool = true;
                break;
            }
        }

        if (isBuildTool) {
            Log("DetourFlow DLL Loaded inside build tool %ws. Bypassing all hooks.", exeStr.c_str());
            DetourRestoreAfterWith();
            return TRUE;
        }

        SetEnvironmentVariableA("no_proxy", "localhost,127.0.0.1,::1");
        SetEnvironmentVariableA("NO_PROXY", "localhost,127.0.0.1,::1");
        Log("DetourFlow DLL Loaded inside process. Set local bypass env (no_proxy/NO_PROXY).");

        DetourRestoreAfterWith();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        // Attach Winsock Hooks
        DetourAttach(&(PVOID&)TrueConnect, HookConnect);
        DetourAttach(&(PVOID&)TrueWSAConnect, HookWSAConnect);
        DetourAttach(&(PVOID&)TrueWSAIoctl, HookWSAIoctl);
        DetourAttach(&(PVOID&)TrueClosesocket, HookClosesocket);
        DetourAttach(&(PVOID&)TrueIoctlsocket, HookIoctlsocket);
        DetourAttach(&(PVOID&)TrueWSAEventSelect, HookWSAEventSelect);
        DetourAttach(&(PVOID&)TrueGetAddrInfoW, HookGetAddrInfoW);
        DetourAttach(&(PVOID&)TrueFreeAddrInfoW, HookFreeAddrInfoW);

        // Attach Process hooks
        DetourAttach(&(PVOID&)TrueCreateProcessA, HookCreateProcessA);
        DetourAttach(&(PVOID&)TrueCreateProcessW, HookCreateProcessW);

        // Attach Kernel32 IOCP Hooks
        DetourAttach(&(PVOID&)TrueGetQueuedCompletionStatus, HookGetQueuedCompletionStatus);
        DetourAttach(&(PVOID&)TrueGetQueuedCompletionStatusEx, HookGetQueuedCompletionStatusEx);
        DetourAttach(&(PVOID&)TrueCreateIoCompletionPort, HookCreateIoCompletionPort);

        DetourTransactionCommit();
        Log("All detours successfully committed.");
    } 
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        Log("DetourFlow DLL Detached from process.");

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        // Detach Winsock Hooks
        DetourDetach(&(PVOID&)TrueConnect, HookConnect);
        DetourDetach(&(PVOID&)TrueWSAConnect, HookWSAConnect);
        DetourDetach(&(PVOID&)TrueWSAIoctl, HookWSAIoctl);
        DetourDetach(&(PVOID&)TrueClosesocket, HookClosesocket);
        DetourDetach(&(PVOID&)TrueIoctlsocket, HookIoctlsocket);
        DetourDetach(&(PVOID&)TrueWSAEventSelect, HookWSAEventSelect);
        DetourDetach(&(PVOID&)TrueGetAddrInfoW, HookGetAddrInfoW);
        DetourDetach(&(PVOID&)TrueFreeAddrInfoW, HookFreeAddrInfoW);

        // Detach Process hooks
        DetourDetach(&(PVOID&)TrueCreateProcessA, HookCreateProcessA);
        DetourDetach(&(PVOID&)TrueCreateProcessW, HookCreateProcessW);

        // Detach Kernel32 IOCP Hooks
        DetourDetach(&(PVOID&)TrueGetQueuedCompletionStatus, HookGetQueuedCompletionStatus);
        DetourDetach(&(PVOID&)TrueGetQueuedCompletionStatusEx, HookGetQueuedCompletionStatusEx);
        DetourDetach(&(PVOID&)TrueCreateIoCompletionPort, HookCreateIoCompletionPort);

        DetourTransactionCommit();

        // Free remaining structures in global maps
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& pair : g_PendingConnectEx) {
            if (pair.second.sendBuffer) {
                delete[] pair.second.sendBuffer;
            }
        }
        g_PendingConnectEx.clear();
        g_RedirectedSockets.clear();
        g_SocketNonBlockingState.clear();

        for (PADDRINFOW p : g_MyAllocatedAddrInfo) {
            HeapFree(GetProcessHeap(), 0, p);
        }
        g_MyAllocatedAddrInfo.clear();
        g_FakeIpToDomain.clear();
        Log("Detours detached cleanup complete.");
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void DetourDummyExport() {
    // Dummy export to create an export table
}
