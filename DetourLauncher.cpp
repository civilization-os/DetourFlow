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
        printf("错误: 无法解析 DLL 路径格式\n");
        return 1;
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

    // 3. 使用 Detours 创建注入进程
    STARTUPINFOW sInfo = { 0 };
    sInfo.cb = sizeof(sInfo);
    PROCESS_INFORMATION pInfo = { 0 };

    // 使用 DetourCreateProcessWithDllExW 启动挂起并注入
    BOOL success = DetourCreateProcessWithDllExW(
        NULL,
        const_cast<LPWSTR>(commandLine.c_str()),
        NULL,
        NULL,
        TRUE,
        CREATE_DEFAULT_ERROR_MODE,
        NULL,
        NULL,
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

    // 4. 等待进程退出并获取退出码
    WaitForSingleObject(pInfo.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pInfo.hProcess, &exitCode);

    CloseHandle(pInfo.hProcess);
    CloseHandle(pInfo.hThread);

    printf("[+] 目标进程已退出，退出码: %lu\n", exitCode);
    return exitCode;
}
