// test/selfcheck.cpp
//
// 自校验：验证 PROC_THREAD_ATTRIBUTE_PARENT_PROCESS 确实能把 A 的父进程换成 B，
// 且全程不修改系统状态、不接触游戏。
//
// 编译：
//   g++ -std=c++20 -O2 -municode -o selfcheck.exe selfcheck.cpp -lwintrust
//
// 运行：
//   selfcheck.exe          编排：起"假父进程"，再把"假子进程"挂到它下面
//   selfcheck.exe --fake-parent            内部角色：只睡、什么都不做
//   selfcheck.exe --fake-child <期望父PID>  内部角色：报告自己的真实父 PID
//
// 退出码 0 = 父进程链构造成功；1 = 失败。

#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// CreateToolhelp32Snapshot 在进程刚创建时可能返回 ERROR_BAD_LENGTH，必须重试。
DWORD QueryParentPid(DWORD pid) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            DWORD found = 0;
            if (Process32FirstW(snap, &entry)) {
                do {
                    if (entry.th32ProcessID == pid) {
                        found = entry.th32ParentProcessID;
                        break;
                    }
                } while (Process32NextW(snap, &entry));
            }
            CloseHandle(snap);
            if (found != 0) return found;
        }
        Sleep(25);
    }
    return 0;
}

std::wstring SelfPath() {
    wchar_t buffer[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH * 2);
    return buffer;
}

std::wstring TempPathFor(const wchar_t* tag) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    wchar_t name[128] = {};
    swprintf_s(name, L"eq_selfcheck_%s_%lu.txt", tag, GetCurrentProcessId());
    return std::wstring(dir) + name;
}

// ---- 角色：只会睡觉的"假父进程" -------------------------------------------
int RunFakeParent() {
    // 真实官方 Games.exe 被挂起时也是"活着但一行代码不执行"，这里用等事件近似。
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\EqSelfCheckStop");
    if (stop) {
        WaitForSingleObject(stop, 30000);
        CloseHandle(stop);
    } else {
        Sleep(15000);
    }
    return 0;
}

// ---- 角色：报告自己父进程的"假子进程" --------------------------------------
int RunFakeChild() {
    wchar_t expectedText[32] = {};
    GetEnvironmentVariableW(L"EQ_SELFCHECK_EXPECTED", expectedText, 32);
    const DWORD want = wcstoul(expectedText, nullptr, 10);

    const DWORD parent = QueryParentPid(GetCurrentProcessId());

    wchar_t outPath[MAX_PATH * 2] = {};
    GetEnvironmentVariableW(L"EQ_SELFCHECK_OUT", outPath, MAX_PATH * 2);
    if (outPath[0] != L'\0') {
        FILE* file = nullptr;
        if (_wfopen_s(&file, outPath, L"w") == 0 && file) {
            fwprintf(file, L"pid=%lu\nparent=%lu\nexpected=%lu\nmatch=%d\n",
                     GetCurrentProcessId(), parent, want, parent == want ? 1 : 0);
            fclose(file);
        }
    }
    return (parent == want) ? 0 : 3;
}

// 统一按 UTF-8 字节写出，避免重定向 / MSYS 终端下的编码错乱。
void Print(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wchar_t buffer[2048] = {};
    vswprintf_s(buffer, fmt, args);
    va_end(args);

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8.data(), bytes, nullptr, nullptr);
        fwrite(utf8.data(), 1, utf8.size(), stdout);
        fflush(stdout);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && _wcsicmp(argv[1], L"--fake-parent") == 0) return RunFakeParent();
    if (argc >= 2 && _wcsicmp(argv[1], L"--fake-child") == 0) return RunFakeChild();

    const std::wstring self = SelfPath();
    const std::wstring outPath = TempPathFor(L"child");

    // --- 确保 --fake-parent 的"名字"看起来像个独立进程，这里直接复用自身 exe ---
    STARTUPINFOW parentSi{};
    parentSi.cb = sizeof(parentSi);
    parentSi.dwFlags = STARTF_USESHOWWINDOW;
    parentSi.wShowWindow = SW_HIDE;

    std::wstring parentCmd = Quote(self) + L" --fake-parent";
    PROCESS_INFORMATION parentPi{};

    Print(L"[1/3] 以 CREATE_SUSPENDED 启动假父进程 ...\n");
    if (!CreateProcessW(self.c_str(), parentCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                        &parentSi, &parentPi)) {
        Print(L"      失败：CreateProcessW error %lu\n", GetLastError());
        return 1;
    }
    Print(L"      PID = %lu（保持挂起）\n", parentPi.dwProcessId);

    // --- 属性列表：把子进程的父进程指定为上面那个挂起的进程 ---
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        Print(L"      失败：InitializeProcThreadAttributeList error %lu\n", GetLastError());
        TerminateProcess(parentPi.hProcess, 0);
        return 1;
    }
    HANDLE parentHandle = parentPi.hProcess;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                                   &parentHandle, sizeof(parentHandle), nullptr, nullptr)) {
        Print(L"      失败：UpdateProcThreadAttribute error %lu\n", GetLastError());
        DeleteProcThreadAttributeList(attrs);
        TerminateProcess(parentPi.hProcess, 0);
        return 1;
    }

    // --- 子进程：告诉它期望的父 PID，让它自己查证后写文件 ---
    wchar_t expectedText[32] = {};
    swprintf_s(expectedText, L"%lu", parentPi.dwProcessId);
    SetEnvironmentVariableW(L"EQ_SELFCHECK_EXPECTED", expectedText);
    SetEnvironmentVariableW(L"EQ_SELFCHECK_OUT", outPath.c_str());

    Print(L"[2/3] 用 PROC_THREAD_ATTRIBUTE_PARENT_PROCESS 启动假子进程 ...\n");
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.lpAttributeList = attrs;

    // 目标本身是控制台子系统：伪造父进程下会继承不到控制台而 0xC0000142 秒退，
    // 因此补 CREATE_NEW_CONSOLE（Endfield.exe 是 GUI 子系统，不需要这个标志）。
    std::wstring childCmd = Quote(self) + L" --fake-child";
    PROCESS_INFORMATION childPi{};
    if (!CreateProcessW(self.c_str(), childCmd.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                            CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                        nullptr, nullptr, &si.StartupInfo, &childPi)) {
        Print(L"      失败：CreateProcessW error %lu\n", GetLastError());
        DeleteProcThreadAttributeList(attrs);
        TerminateProcess(parentPi.hProcess, 0);
        return 1;
    }
    ResumeThread(childPi.hThread);
    Print(L"      PID = %lu\n", childPi.dwProcessId);

    Print(L"[3/3] 等子进程自证父进程 ...\n");
    WaitForSingleObject(childPi.hProcess, 15000);
    DWORD exitCode = 0;
    GetExitCodeProcess(childPi.hProcess, &exitCode);

    std::wstring report;
    FILE* file = nullptr;
    if (_wfopen_s(&file, outPath.c_str(), L"r") == 0 && file) {
        wchar_t line[512] = {};
        while (fgetws(line, 512, file)) report += line;
        fclose(file);
    }
    DeleteFileW(outPath.c_str());

    Print(L"\n子进程自报：\n%s\n", report.empty() ? L"(没有报告)\n" : report.c_str());

    // 清理：不管成败都不留进程
    SetEnvironmentVariableW(L"EQ_SELFCHECK_EXPECTED", nullptr);
    SetEnvironmentVariableW(L"EQ_SELFCHECK_OUT", nullptr);
    HANDLE stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\EqSelfCheckStop");
    if (stopEvent) { SetEvent(stopEvent); CloseHandle(stopEvent); }
    Sleep(120);
    TerminateProcess(parentPi.hProcess, 0);
    CloseHandle(parentPi.hThread);
    CloseHandle(parentPi.hProcess);
    CloseHandle(childPi.hThread);
    CloseHandle(childPi.hProcess);
    DeleteProcThreadAttributeList(attrs);

    const bool pass = report.find(L"match=1") != std::wstring::npos;
    Print(pass ? L"结果：PASS —— 子进程的直接父进程就是那个隐藏挂起的进程。\n"
               : L"结果：FAIL —— 父进程链没有建立成功。\n");
    return pass ? 0 : 1;
}
