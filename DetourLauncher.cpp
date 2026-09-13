#include <windows.h>
#include <stdio.h>
#include <detours.h>
#include <string>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    // 设置控制台输出编码为 UTF-8，解决 PowerShell 乱码问题
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        wprintf(L"使用方法: %s <目标可执行文件路径> [参数1 参数2 ...]\n", argv[0]);
        wprintf(L"示例: %s \"C:\\Windows\\System32\\curl.exe\" https://www.google.com\n", argv[0]);
        return 1;
    }

    // 1. 获取同目录下的 DetourFlow.dll 的绝对路径
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    std::wstring dllPath = exeDir + L"\\DetourFlow.dll";

    // 转换为 ANSI 编码的路径，因为 DetourCreateProcessWithDllExW 接收 LPCSTR lpDllName
    char dllPathA[MAX_PATH];
    int size = WideCharToMultiByte(CP_ACP, 0, dllPath.c_str(), -1, dllPathA, MAX_PATH, NULL, NULL);
    if (size <= 0) {
        // CP_ACP 转换失败时尝试短路径（解决 Unicode 路径在非中文系统的乱码问题）
        wchar_t shortPath[MAX_PATH];
        DWORD shortLen = GetShortPathNameW(dllPath.c_str(), shortPath, MAX_PATH);
        if (shortLen > 0 && shortLen < MAX_PATH) {
            size = WideCharToMultiByte(CP_ACP, 0, shortPath, -1, dllPathA, MAX_PATH, NULL, NULL);
        }
        if (size <= 0) {
            printf("错误: 无法解析 DLL 路径格式\n");
            return 1;
        }
    }

    // 检查 DLL 文件是否存在
    DWORD attribs = GetFileAttributesA(dllPathA);
    if (attribs == INVALID_FILE_ATTRIBUTES || (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("错误: 找不到 DLL 文件: %s\n", dllPathA);
        return 1;
    }

    // 2. 组装目标的命令行参数
    std::wstring commandLine = L"";
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        // 如果参数包含空格且没被双引号包裹，则用双引号包起来
        if (arg.find(L' ') != std::wstring::npos && arg.front() != L'"') {
            arg = L"\"" + arg + L"\"";
        }
        commandLine += arg + (i == argc - 1 ? L"" : L" ");
    }

    wprintf(L"[*] 正在启动目标: %s\n", argv[1]);
    wprintf(L"[*] 命令行参数: %s\n", commandLine.c_str());
    printf("[*] 注入 DLL 路径: %s\n", dllPathA);

    // 3. 解析目标程序所在目录作为工作目录，防止依赖自身资源的程序启动失败
    std::wstring targetExe = argv[1];
    std::wstring targetDir = L"";
    size_t targetSlash = targetExe.find_last_of(L"\\/");
    if (targetSlash != std::wstring::npos) {
        targetDir = targetExe.substr(0, targetSlash);
    }
    if (!targetDir.empty()) {
        wprintf(L"[*] 工作目录: %s\n", targetDir.c_str());
    }

    // 4. 使用 Detours 创建注入进程
    STARTUPINFOW sInfo = { 0 };
    sInfo.cb = sizeof(sInfo);
    PROCESS_INFORMATION pInfo = { 0 };

    // 使用 DetourCreateProcessWithDllExW 启动挂起并注入
    // DetourCreateProcessWithDllExW takes LPWSTR (may modify it per CreateProcessW contract),
    // so we use the mutable buffer from std::wstring (&commandLine[0]) instead of const_cast.
    BOOL success = DetourCreateProcessWithDllExW(
        NULL,
        &commandLine[0],
        NULL,
        NULL,
        TRUE,
        CREATE_DEFAULT_ERROR_MODE,
        NULL,
        targetDir.empty() ? NULL : targetDir.c_str(),
        &sInfo,
        &pInfo,
        dllPathA,
        NULL
    );

    if (!success) {
        DWORD err = GetLastError();
        printf("错误: 无法启动目标进程并注入 (错误码: %lu)\n", err);
        return 1;
    }

    printf("[+] 目标进程启动成功。PID: %lu\n", pInfo.dwProcessId);
    printf("[DETOUR_TARGET_PID] %lu\n", pInfo.dwProcessId);
    fflush(stdout);

    // 5. 等待进程退出并获取退出码
    WaitForSingleObject(pInfo.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pInfo.hProcess, &exitCode);

    CloseHandle(pInfo.hProcess);
    CloseHandle(pInfo.hThread);

    wprintf(L"[*] 目标进程 (PID: %lu) 已退出，退出码: %lu\n", pInfo.dwProcessId, exitCode);
    return (int)exitCode;
}
