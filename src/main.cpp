// ============================================================================
//  EndfieldQuickLaunch - 免启动器快速启动器
//
//  原理（复刻 Better-Endfield 的 official_launcher 机制）：
//    1. 在本机找到真正签名过的官方启动器 Games.exe
//    2. 以 CREATE_SUSPENDED | CREATE_NO_WINDOW 启动它，并且永不恢复其主线程
//       -> 进程存在、签名可验证，但不会显示任何界面
//    3. 用 PROC_THREAD_ATTRIBUTE_PARENT_PROCESS 把这个挂起进程设成
//       Endfield.exe 的父进程，骗过 OneChannelLC/U8SDK 的父进程检查
//    4. 游戏以挂起方式创建，注入可选的 DLL，然后恢复运行
//    5. 游戏退出后清理自建的父进程
//
//  构建：build.bat   （需要 MinGW-w64 g++）
// ============================================================================

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- 控制台输出
//
// 本程序以 GUI 子系统编译（双击运行不会有黑框），所以输出分三种情况：
//   1. 已经连着控制台（从 cmd / PowerShell 启动）-> 直接 WriteConsoleW
//   2. 标准输出被重定向到文件 / 管道  -> 写 UTF-8 字节
//   3. 什么都没有（双击启动）        -> 先缓存，退出时写 %LOCALAPPDATA% 下的 last-run.log
static bool g_hasConsole = false;
static std::wstring g_captured;

static std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::string();
    std::string utf8(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), required, nullptr, nullptr);
    return utf8;
}

static bool IsConsoleHandle(HANDLE handle) {
    DWORD mode = 0;
    return handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
}

static void InitOutput() {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (IsConsoleHandle(output)) { g_hasConsole = true; return; }
    if (output && output != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(output);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;  // 重定向，直接写句柄
    }
    // 双击启动时父进程（explorer）没有控制台，这里会失败，于是退化为只写日志。
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    const HANDLE console = CreateFileW(L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) return;
    SetStdHandle(STD_OUTPUT_HANDLE, console);
    SetStdHandle(STD_ERROR_HANDLE, console);
    g_hasConsole = true;
}

static void Print(const std::wstring& text) {
    if (text.empty()) return;
    if (g_hasConsole) {
        DWORD written = 0;
        if (WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), text.c_str(),
                static_cast<DWORD>(text.size()), &written, nullptr))
            return;
    }
    const std::string utf8 = ToUtf8(text);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!utf8.empty() && output && output != INVALID_HANDLE_VALUE &&
        GetFileType(output) != FILE_TYPE_CHAR) {
        DWORD written = 0;
        WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        return;
    }
    g_captured += text;   // 没地方可写（或控制台写失败）就先存着，退出时落日志
}

static void PrintLine(const std::wstring& text = L"") { Print(text + L"\n"); }

// 双击启动时没有任何窗口：把这次运行的输出落到日志文件里，方便排查。
// 任何时候都不弹窗（成功失败都静默），需要看输出时从 cmd / PowerShell 运行，
// 或者打开 %LOCALAPPDATA%\EndfieldQuickLaunch\last-run.log。
static void WriteRunLog(const std::wstring& text) {
    wchar_t localAppData[32768]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
            static_cast<DWORD>(std::size(localAppData)))) return;
    const fs::path directory = fs::path(localAppData) / L"EndfieldQuickLaunch";
    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path file = directory / L"last-run.log";
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"wb") != 0 || !stream) return;
    // UTF-8 BOM，记事本打开不乱码
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, sizeof(bom), stream);
    const std::string utf8 = ToUtf8(text);
    if (!utf8.empty()) fwrite(utf8.data(), 1, utf8.size(), stream);
    fclose(stream);
}

static int Finish(int code) {
    // 控制台正常时 g_captured 一直是空的，只有"没地方显示"的那部分内容才会落到日志里。
    if (!g_captured.empty()) WriteRunLog(g_captured);
    g_captured.clear();
    return code;
}

static std::wstring WinError(const wchar_t* operation, DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = std::wstring(operation) + L" (Win32 " +
        std::to_wstring(code) + L")";
    if (message) {
        std::wstring detail(message);
        while (!detail.empty() &&
               (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' '))
            detail.pop_back();
        if (!detail.empty()) result += L": " + detail;
        LocalFree(message);
    }
    return result;
}

// ------------------------------------------------------------------ 基础工具

// WinVerifyTrust / Win32 状态值统一格式化成 0xXXXXXXXX，便于贴到日志里搜。
static std::wstring StatusHex(DWORD status) {
    wchar_t text[24]{};
    swprintf(text, 24, L"0x%08lX", static_cast<unsigned long>(status));
    return text;
}

static bool IEquals(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool IStartsWith(const std::wstring& text, const wchar_t* prefix) {
    return _wcsnicmp(text.c_str(), prefix, wcslen(prefix)) == 0;
}

static std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
        } else {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

// 统一的命令行拼装：可执行文件 + 参数。多处用到，避免各写一遍走样。
static std::wstring CommandLineFor(const fs::path& executable,
    const std::vector<std::wstring>& arguments) {
    std::wstring command = QuoteArg(executable.wstring());
    for (const auto& argument : arguments) command += L" " + QuoteArg(argument);
    return command;
}

static fs::path ModulePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return fs::path(std::wstring(buffer.data(), length));
}

static bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION info{};
        DWORD size = sizeof(info);
        if (GetTokenInformation(token, TokenElevation, &info, sizeof(info), &size))
            elevated = info.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// -------------------------------------------------------------------- 注册表

struct RegKey {
    HKEY value = nullptr;
    ~RegKey() { if (value) RegCloseKey(value); }
};

static std::wstring RegRead(HKEY root, const wchar_t* key, const wchar_t* name,
    REGSAM view = 0) {
    RegKey opened;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &opened.value) != ERROR_SUCCESS)
        return {};
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(opened.value, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536)
        return {};
    std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(opened.value, name, nullptr, nullptr,
            reinterpret_cast<BYTE*>(text.data()), &bytes) != ERROR_SUCCESS)
        return {};
    if (type != REG_EXPAND_SZ) return text.data();
    const DWORD required = ExpandEnvironmentStringsW(text.data(), nullptr, 0);
    if (!required || required > 32768) return {};
    std::vector<wchar_t> expanded(required);
    if (!ExpandEnvironmentStringsW(text.data(), expanded.data(), required)) return {};
    return expanded.data();
}

static std::vector<std::wstring> RegSubkeys(HKEY root, const wchar_t* path,
    REGSAM view = 0) {
    RegKey opened;
    std::vector<std::wstring> names;
    if (RegOpenKeyExW(root, path, 0, KEY_ENUMERATE_SUB_KEYS | view, &opened.value) != ERROR_SUCCESS)
        return names;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD size = static_cast<DWORD>(std::size(name));
        const LSTATUS status = RegEnumKeyExW(opened.value, index, name, &size,
            nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS) names.emplace_back(name, size);
    }
    return names;
}

// ---------------------------------------------------------------- 版本号解析

static std::vector<unsigned long> ParseVersion(const fs::path& path) {
    const std::wstring name = path.parent_path().filename().wstring();
    if (name.empty() || name.find_first_not_of(L"0123456789.") != std::wstring::npos)
        return {};
    std::vector<unsigned long> result;
    size_t offset = 0;
    try {
        while (offset < name.size()) {
            size_t consumed = 0;
            result.push_back(std::stoul(name.substr(offset), &consumed));
            offset += consumed;
            if (offset < name.size() && name[offset++] != L'.') return {};
        }
    } catch (...) {
        return {};
    }
    return result;
}

// -------------------------------------------------------------- PE 与签名

static bool IsPeImage(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD bytes = 0;
    bool ok = ReadFile(file, &dos, sizeof(dos), &bytes, nullptr) && bytes == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0;
    if (ok) {
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN);
        DWORD signature = 0;
        ok = ReadFile(file, &signature, sizeof(signature), &bytes, nullptr) &&
            bytes == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
    }
    CloseHandle(file);
    return ok;
}

// 读取 PE 可选头的子系统字段（2=GUI, 3=控制台），失败返回 0。
static unsigned short PeSubsystem(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    IMAGE_DOS_HEADER dos{};
    DWORD bytes = 0;
    unsigned short subsystem = 0;
    if (ReadFile(file, &dos, sizeof(dos), &bytes, nullptr) && bytes == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        DWORD signature = 0;
        if (ReadFile(file, &signature, sizeof(signature), &bytes, nullptr) &&
            bytes == sizeof(signature) && signature == IMAGE_NT_SIGNATURE) {
            IMAGE_FILE_HEADER header{};
            if (ReadFile(file, &header, sizeof(header), &bytes, nullptr) &&
                bytes == sizeof(header)) {
                IMAGE_OPTIONAL_HEADER64 optional{};
                const DWORD wanted = header.SizeOfOptionalHeader < sizeof(optional)
                    ? header.SizeOfOptionalHeader : static_cast<DWORD>(sizeof(optional));
                if (ReadFile(file, &optional, wanted, &bytes, nullptr) &&
                    bytes >= 70 && optional.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                    subsystem = optional.Subsystem;
            }
        }
    }
    CloseHandle(file);
    return subsystem;
}

static std::wstring QuerySigner(const fs::path& path) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0, contentType = 0, format = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType, &format,
            &store, &message, nullptr))
        return {};

    std::wstring signer;
    DWORD infoSize = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &infoSize) && infoSize) {
        std::vector<BYTE> info(infoSize);
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, info.data(), &infoSize)) {
            auto* signerInfo = reinterpret_cast<CMSG_SIGNER_INFO*>(info.data());
            CERT_INFO certInfo{};
            certInfo.Issuer = signerInfo->Issuer;
            certInfo.SerialNumber = signerInfo->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(store,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &certInfo, nullptr);
            if (cert) {
                wchar_t name[512]{};
                if (CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                        name, static_cast<DWORD>(std::size(name))))
                    signer = name;
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
    return signer;
}

struct TrustResult {
    bool trusted = false;
    LONG status = 0;
    std::wstring signer;
};

static TrustResult VerifyAuthenticode(const fs::path& path) {
    TrustResult result;
    std::wstring file = path.wstring();
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = file.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    result.status = WinVerifyTrust(nullptr, &policy, &data);
    result.trusted = result.status == ERROR_SUCCESS;

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);

    if (result.trusted) result.signer = QuerySigner(path);
    return result;
}

// ------------------------------------------------------------ 官方启动器发现

static void AddRoot(std::vector<fs::path>& roots, const fs::path& path) {
    if (path.empty()) return;
    fs::path root = path;
    if (root.has_extension() && IEquals(root.extension().wstring(), L".exe"))
        root = root.parent_path();
    if (std::none_of(roots.begin(), roots.end(),
            [&](const fs::path& existing) { return IEquals(existing.wstring(), root.wstring()); }))
        roots.push_back(root);
}

static std::vector<fs::path> InstalledLauncherRoots() {
    std::vector<fs::path> roots;

    // 1) 协议注册表：hypergryphlauncher* / gryphlinklauncher*
    for (const auto& protocol : RegSubkeys(HKEY_CLASSES_ROOT, L"")) {
        if (!IStartsWith(protocol, L"hypergryphlauncher") &&
            !IStartsWith(protocol, L"gryphlinklauncher"))
            continue;
        const auto command = RegRead(HKEY_CLASSES_ROOT,
            (protocol + L"\\shell\\open\\command").c_str(), nullptr);
        if (command.empty()) continue;
        int count = 0;
        wchar_t** arguments = CommandLineToArgvW(command.c_str(), &count);
        if (arguments) {
            if (count > 0) AddRoot(roots, arguments[0]);
            LocalFree(arguments);
        }
    }

    // 2) 卸载表里的 DisplayName
    constexpr wchar_t uninstall[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {static_cast<REGSAM>(KEY_WOW64_64KEY),
                            static_cast<REGSAM>(KEY_WOW64_32KEY)}) {
            for (const auto& child : RegSubkeys(hive, uninstall, view)) {
                const std::wstring key = std::wstring(uninstall) + L"\\" + child;
                const auto name = RegRead(hive, key.c_str(), L"DisplayName", view);
                if (!IEquals(name, L"鹰角启动器") && !IEquals(name, L"Hypergryph Launcher") &&
                    !IEquals(name, L"GRYPHLINK") && !IEquals(name, L"Gryphlink") &&
                    !IEquals(name, L"Gryphline Launcher"))
                    continue;
                AddRoot(roots, RegRead(hive, key.c_str(), L"InstallLocation", view));
            }
        }
    }

    // 3) 常见安装目录
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        wchar_t value[32768]{};
        const DWORD length = GetEnvironmentVariableW(variable, value,
            static_cast<DWORD>(std::size(value)));
        if (length && length < std::size(value)) {
            AddRoot(roots, fs::path(value) / L"Hypergryph Launcher");
            AddRoot(roots, fs::path(value) / L"GRYPHLINK");
        }
    }
    return roots;
}

static bool ResolveToGamesCore(const fs::path& input, fs::path& resolved) {
    std::error_code ec;
    fs::path folder = input;
    if (fs::is_regular_file(input, ec)) {
        if (IEquals(input.filename().wstring(), L"Games.exe")) {
            resolved = input;
            return true;
        }
        folder = input.parent_path();
    }
    std::vector<fs::path> candidates;
    if (fs::is_directory(folder, ec)) {
        if (fs::is_regular_file(folder / L"Games.exe", ec))
            candidates.push_back(folder / L"Games.exe");
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const auto games = it->path() / L"Games.exe";
            if (!ParseVersion(games).empty() && fs::is_regular_file(games, ec))
                candidates.push_back(games);
        }
    }
    if (!candidates.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const fs::path& a, const fs::path& b) { return ParseVersion(a) > ParseVersion(b); });
        resolved = candidates.front();
        return true;
    }
    if (fs::is_regular_file(input, ec)) {
        resolved = input;
        return true;
    }
    return false;
}

static std::vector<fs::path> CollectLauncherCandidates() {
    std::vector<fs::path> candidates;
    for (const auto& root : InstalledLauncherRoots()) {
        std::error_code ec;
        if (fs::is_regular_file(root / L"Games.exe", ec))
            candidates.push_back(root / L"Games.exe");
        if (fs::is_regular_file(root / L"Launcher.exe", ec))
            candidates.push_back(root / L"Launcher.exe");
        for (fs::directory_iterator it(root, ec), end;
             !ec && it != end; it.increment(ec)) {
            const auto games = it->path() / L"Games.exe";
            if (!ParseVersion(games).empty() && fs::is_regular_file(games, ec))
                candidates.push_back(games);
            const auto launcher = it->path() / L"Launcher.exe";
            if (!ParseVersion(launcher).empty() && fs::is_regular_file(launcher, ec))
                candidates.push_back(launcher);
            ec.clear();
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const fs::path& a, const fs::path& b) { return ParseVersion(a) > ParseVersion(b); });
    return candidates;
}

// -------------------------------------------------------------- 游戏路径发现

static bool LooksLikeSystemDir(const std::wstring& name) {
    static const wchar_t* blocked[] = {
        L"Windows", L"$Recycle.Bin", L"System Volume Information", L"Recovery",
        L"PerfLogs", L"Windows.old", L"node_modules", L".git", L"Temp", L"tmp",
        L"Microsoft", L"Packages", L"WinSxS", L"ProgramData"};
    for (const auto* blocked_name : blocked)
        if (IEquals(name, blocked_name)) return true;
    return false;
}

static void SearchDirectoryForGame(const fs::path& directory, int depth,
    int& budget, std::vector<fs::path>& found) {
    if (depth < 0 || budget <= 0) return;
    std::error_code ec;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && budget > 0; it.increment(ec)) {
        --budget;
        const fs::path& path = it->path();
        const std::wstring name = path.filename().wstring();
        std::error_code entryError;
        if (it->is_directory(entryError)) {
            if (LooksLikeSystemDir(name)) continue;
            if (fs::is_regular_file(path / L"Endfield.exe", entryError))
                found.push_back(path / L"Endfield.exe");
            if (depth > 0) SearchDirectoryForGame(path, depth - 1, budget, found);
        } else if (IEquals(name, L"Endfield.exe")) {
            found.push_back(path);
        }
        if (found.size() >= 8) return;
    }
}

static fs::path FindGameExecutable() {
    std::vector<fs::path> found;
    // 两个阶段共享同一份预算，保证最坏情况也是有限次目录枚举后返回。
    int budget = 40000;

    // 1) 卸载表里名字带 Endfield 的条目
    constexpr wchar_t uninstall[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {static_cast<REGSAM>(KEY_WOW64_64KEY),
                            static_cast<REGSAM>(KEY_WOW64_32KEY)}) {
            for (const auto& child : RegSubkeys(hive, uninstall, view)) {
                if (budget <= 0) break;
                const std::wstring key = std::wstring(uninstall) + L"\\" + child;
                const auto name = RegRead(hive, key.c_str(), L"DisplayName", view);
                if (name.empty()) continue;
                if (name.find(L"Endfield") == std::wstring::npos &&
                    name.find(L"终末地") == std::wstring::npos)
                    continue;
                auto location = RegRead(hive, key.c_str(), L"InstallLocation", view);
                if (location.empty()) continue;
                // 安装目录本身或最多三层子目录里找，复用同一套扫描逻辑。
                const fs::path directory(location);
                std::error_code ec;
                if (fs::is_regular_file(directory / L"Endfield.exe", ec))
                    found.push_back(directory / L"Endfield.exe");
                else
                    SearchDirectoryForGame(directory, 3, budget, found);
            }
        }
    }

    // 2) 兜底：有限深度的目录扫描
    if (found.empty()) {
        std::vector<fs::path> startPoints;
        for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
            wchar_t value[32768]{};
            const DWORD length = GetEnvironmentVariableW(variable, value,
                static_cast<DWORD>(std::size(value)));
            if (length && length < std::size(value)) startPoints.emplace_back(value);
        }
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            wchar_t root[4] = {drive, L':', L'\\', L'\0'};
            if (GetDriveTypeW(root) == DRIVE_FIXED) startPoints.emplace_back(root);
        }
        for (const auto& start : startPoints) {
            if (budget <= 0) break;
            SearchDirectoryForGame(start, 3, budget, found);
            if (!found.empty()) break;
        }
    }

    if (found.empty()) return {};
    // 多处安装时选最近改动过的那份（更可能是当前在玩的版本）；
    // 时间相同再按路径字典序，保证结果稳定可复现。
    auto modifiedAt = [](const fs::path& path) {
        std::error_code error;
        const auto time = fs::last_write_time(path, error);
        return error ? fs::file_time_type::min() : time;
    };
    std::stable_sort(found.begin(), found.end(),
        [&](const fs::path& left, const fs::path& right) {
            const auto leftTime = modifiedAt(left), rightTime = modifiedAt(right);
            if (leftTime != rightTime) return leftTime > rightTime;
            return left < right;
        });
    return found.front();
}

// ---------------------------------------------------------------- 配置文件

static fs::path SettingsDirectory() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
        static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return fs::path(buffer) / L"EndfieldQuickLaunch";
}

static fs::path ConfigPath() {
    const auto directory = SettingsDirectory();
    return directory.empty() ? fs::path() : directory / L"config.ini";
}

static std::wstring ReadConfig(const wchar_t* key) {
    const auto path = ConfigPath();
    if (path.empty()) return {};
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"Launch", key, L"", buffer,
        static_cast<DWORD>(std::size(buffer)), path.c_str());
    return buffer;
}

static void WriteConfig(const wchar_t* key, const std::wstring& value) {
    const auto directory = SettingsDirectory();
    if (directory.empty()) return;
    std::error_code ec;
    fs::create_directories(directory, ec);
    const auto path = ConfigPath();
    if (path.empty()) return;
    WritePrivateProfileStringW(L"Launch", key, value.c_str(), path.c_str());
}

// -------------------------------------------------------------- 进程操作封装

using CreateProcessW_Fn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
    LPPROCESS_INFORMATION);

// 绕过 Steam overlay 对 kernel32!CreateProcessW 的内联 hook，让隐藏的父进程不被 Steam 捕获。
// 判断依据是 kernelbase!CreateProcessW 自身的首字节：没被 hook 就直连 kernelbase
// （跳过 kernel32 上的转发函数），被 hook 了才退回 kernel32 的正常导出。
static BOOL CreateProcessWithoutOverlayHook(LPCWSTR applicationName,
    LPWSTR commandLine, LPSECURITY_ATTRIBUTES processAttributes,
    LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles,
    DWORD creationFlags, LPVOID environment, LPCWSTR currentDirectory,
    LPSTARTUPINFOW startupInfo, LPPROCESS_INFORMATION processInformation) {
    if (HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll")) {
        auto createProcess = reinterpret_cast<CreateProcessW_Fn>(
            reinterpret_cast<void*>(GetProcAddress(kernelbase, "CreateProcessW")));
        if (createProcess) {
            const auto bytes = reinterpret_cast<const unsigned char*>(createProcess);
            if (bytes[0] != 0xE9 && !(bytes[0] == 0xFF && bytes[1] == 0x25) &&
                bytes[0] != 0xEB)
                return createProcess(applicationName, commandLine, processAttributes,
                    threadAttributes, inheritHandles, creationFlags, environment,
                    currentDirectory, startupInfo, processInformation);
        }
    }
    return CreateProcessW(applicationName, commandLine, processAttributes,
        threadAttributes, inheritHandles, creationFlags, environment,
        currentDirectory, startupInfo, processInformation);
}

class OfficialParent {
public:
    ~OfficialParent() { Cleanup(); }

    bool Start(const fs::path& executable, std::wstring& error) {
        if (process_) { error = L"父进程已经启动过了。"; return false; }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION info{};
        std::wstring command = QuoteArg(executable.wstring());
        if (!CreateProcessWithoutOverlayHook(executable.c_str(), command.data(),
                nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                nullptr, executable.parent_path().c_str(), &startup, &info)) {
            error = WinError(L"创建官方启动器父进程失败", GetLastError());
            return false;
        }
        process_ = info.hProcess;
        thread_ = info.hThread;   // 永不 Resume，句柄保留到析构即可
        return true;
    }

    bool CreateGame(const fs::path& game, const std::vector<std::wstring>& arguments,
        DWORD creationFlags, PROCESS_INFORMATION& info, std::wstring& error) const {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size) {
            error = WinError(L"初始化属性列表失败", GetLastError());
            return false;
        }
        std::vector<unsigned char> storage(size);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
            error = WinError(L"初始化属性列表失败", GetLastError());
            return false;
        }
        HANDLE parent = process_;
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                &parent, sizeof(parent), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes);
            error = WinError(L"设置父进程属性失败", GetLastError());
            return false;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_SHOWNORMAL;
        startup.lpAttributeList = attributes;

        std::wstring command = CommandLineFor(game, arguments);

        const BOOL created = CreateProcessW(game.c_str(), command.data(),
            nullptr, nullptr, FALSE, creationFlags | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, game.parent_path().c_str(), &startup.StartupInfo, &info);
        const DWORD lastError = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        if (!created) {
            error = WinError(L"创建游戏进程失败", lastError);
            return false;
        }
        return true;
    }

    bool valid() const { return process_ != nullptr; }
    HANDLE handle() const { return process_; }

    void Cleanup() {
        if (process_) {
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
                TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 5000);   // 等内核回收，最多 5 秒
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (thread_) { CloseHandle(thread_); thread_ = nullptr; }
    }

    // 只放弃句柄，不杀进程。--no-wait 时必须这样做：一旦这里把隐藏的官方父进程
    // 杀掉，游戏稍后再做父进程校验就会失败（10100）。
    void Detach() {
        if (process_) { CloseHandle(process_); process_ = nullptr; }
        if (thread_) { CloseHandle(thread_); thread_ = nullptr; }
    }

private:
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
};

// ------------------------------------------------------------ 远程线程注入

static void* RemoteModuleBaseOnce(DWORD processId, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        processId);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    void* base = nullptr;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (IEquals(entry.szModule, moduleName)) { base = entry.modBaseAddr; break; }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return base;
}

// 进程刚创建时模块快照可能失败（ERROR_BAD_LENGTH），因此带重试。
static void* RemoteModuleBase(DWORD processId, const wchar_t* moduleName,
    DWORD timeoutMs = 5000) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (void* base = RemoteModuleBaseOnce(processId, moduleName)) return base;
        if (GetTickCount64() >= deadline) return nullptr;
        Sleep(100);
    }
}

static bool InjectLibrary(HANDLE process, DWORD processId, const fs::path& dll,
    std::wstring& error) {
    // 32/64 位不匹配时远程线程必然失败，先挡掉，免得给出误导性的“LoadLibraryW 返回失败”。
    BOOL targetWow64 = FALSE, selfWow64 = FALSE;
    IsWow64Process(process, &targetWow64);
    IsWow64Process(GetCurrentProcess(), &selfWow64);
    if (targetWow64 != selfWow64) {
        error = L"目标进程与本程序位数不一致（32/64 位），无法注入 DLL。";
        return false;
    }

    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        error = WinError(L"在目标进程中分配内存失败", GetLastError());
        return false;
    }
    bool ok = WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr) != FALSE;
    if (ok) {
        HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
        auto localLoadLibrary = reinterpret_cast<BYTE*>(
            GetProcAddress(localKernel32, "LoadLibraryW"));
        auto remoteKernel32 = static_cast<BYTE*>(RemoteModuleBase(processId, L"kernel32.dll"));
        if (!localLoadLibrary || !remoteKernel32) {
            error = std::wstring(L"无法定位 LoadLibraryW（本进程 kernel32=") +
                (localLoadLibrary ? L"ok" : L"失败") + L"，目标进程 kernel32=" +
                (remoteKernel32 ? L"ok" : L"失败") + L"）";
            ok = false;
        } else {
            auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                remoteKernel32 + (localLoadLibrary - reinterpret_cast<BYTE*>(localKernel32)));
            HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary,
                remote, 0, nullptr);
            if (!thread) {
                error = WinError(L"创建远程线程失败", GetLastError());
                ok = false;
            } else {
                WaitForSingleObject(thread, 30000);
                DWORD exitCode = 0;
                GetExitCodeThread(thread, &exitCode);
                CloseHandle(thread);
                if (exitCode == 0) {
                    error = L"目标进程 LoadLibraryW 返回失败（DLL 可能不兼容或路径不可访问）。";
                    ok = false;
                }
            }
        }
    } else {
        error = WinError(L"写入远程内存失败", GetLastError());
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

static bool WaitForRemoteModule(DWORD processId, const wchar_t* moduleName,
    DWORD timeoutMs) {
    return RemoteModuleBase(processId, moduleName, timeoutMs) != nullptr;
}

// ------------------------------------------------------------------ 选项解析

struct Options {
    fs::path game;
    fs::path parent;
    fs::path injectDll;
    std::vector<std::wstring> gameArguments;
    bool noParent = false;
    bool dryRun = false;
    bool diagnose = false;
    bool steam = false;
    bool noWait = false;
    bool force = false;
    bool help = false;
    DWORD injectTimeoutMs = 90000;
};

static void PrintUsage() {
    PrintLine(L"EndfieldQuickLaunch - 免启动器快速启动器");
    PrintLine();
    PrintLine(L"用法: EndfieldQuickLaunch.exe [选项] [-- 游戏参数...]");
    PrintLine();
    PrintLine(L"  --game <路径>      Endfield.exe 完整路径（缺省自动查找/读取配置）");
    PrintLine(L"  --parent <路径>    官方启动器 Games.exe 路径（缺省自动查找）");
    PrintLine(L"  --no-parent        不伪造父进程，直接启动（仅适用于不校验的版本）");
    PrintLine(L"  --inject <dll>     启动后向游戏注入指定 DLL（可选）");
    PrintLine(L"  --inject-timeout <毫秒>  等待 GameAssembly.dll 的最长时间，默认 90000");
    PrintLine(L"  --steam            设置 Steam 环境变量（覆盖层需自行处理）");
    PrintLine(L"  --diagnose         只诊断：列出找到的启动器、签名、游戏路径，不启动");
    PrintLine(L"  --dry-run          打印将要执行的命令行，不实际启动");
    PrintLine(L"  --no-wait          启动后立即返回，不等游戏退出");
    PrintLine(L"  --force            即使签名验证失败也继续");
    PrintLine(L"  -h, --help         显示本帮助");
    PrintLine();
    PrintLine(L"示例:");
    PrintLine(L"  EndfieldQuickLaunch.exe");
    PrintLine(L"  EndfieldQuickLaunch.exe --game \"D:\\Games\\Endfield\\Endfield.exe\"");
    PrintLine(L"  EndfieldQuickLaunch.exe --dry-run");
    PrintLine();
    PrintLine(L"配置保存在 %LOCALAPPDATA%\\EndfieldQuickLaunch\\config.ini");
}

static bool ParseOptions(int argc, wchar_t** argv, Options& options,
    std::wstring& error) {
    bool gameArguments = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (gameArguments) { options.gameArguments.push_back(option); continue; }
        if (option == L"--") { gameArguments = true; continue; }

        // 带取值的选项统一处理：缺值、空值都明确报错，而不是静默当成“没写”。
        if (option == L"--game" || option == L"--parent" || option == L"--inject" ||
            option == L"--inject-timeout") {
            if (index + 1 >= argc) { error = L"参数 " + option + L" 缺少取值。"; return false; }
            const std::wstring value = argv[++index];
            if (value.empty()) { error = L"参数 " + option + L" 的取值为空。"; return false; }
            if (option == L"--game") {
                options.game = value;
            } else if (option == L"--parent") {
                options.parent = value;
            } else if (option == L"--inject") {
                options.injectDll = value;
            } else {
                wchar_t* end = nullptr;
                const unsigned long long ms = std::wcstoull(value.c_str(), &end, 10);
                if (!end || *end != L'\0' || ms > 3600000ull) {
                    error = L"--inject-timeout 需要 0..3600000 的整数（毫秒），收到: " + value;
                    return false;
                }
                options.injectTimeoutMs = static_cast<DWORD>(ms);
            }
            continue;
        }
        if (option == L"--no-parent" || option == L"--direct-launch") options.noParent = true;
        else if (option == L"--steam") options.steam = true;
        else if (option == L"--dry-run") options.dryRun = true;
        else if (option == L"--diagnose") options.diagnose = true;
        else if (option == L"--no-wait") options.noWait = true;
        else if (option == L"--force") options.force = true;
        else if (option == L"-h" || option == L"--help" || option == L"/?") options.help = true;
        else { error = L"未知参数: " + option; return false; }
    }
    return true;
}

// ---------------------------------------------------------------------- main

// ------------------------------------------------------- --no-wait 的看护进程
// --no-wait 时主程序马上返回，但隐藏的官方父进程不能跟着消失：游戏稍后还要做
// 父进程校验，父进程没了就是 10100。所以另起一个分离的自身副本，等游戏退出后
// 再清理那个父进程。
static bool SpawnKeeper(DWORD parentPid, DWORD gamePid) {
    const auto self = ModulePath();
    std::wstring command = CommandLineFor(self, {L"--keeper",
        std::to_wstring(parentPid), std::to_wstring(gamePid)});
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE,
            DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT, nullptr,
            self.parent_path().c_str(), &startup, &info))
        return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

static int RunKeeper(DWORD parentPid, DWORD gamePid) {
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    HANDLE parent = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, parentPid);
    if (!parent) { if (game) CloseHandle(game); return 0; }
    if (!game) { TerminateProcess(parent, 0); CloseHandle(parent); return 0; }
    // 句柄在 keeper 启动时就已打开，即使 PID 之后被复用也不会等错对象。
    WaitForSingleObject(game, INFINITE);
    if (WaitForSingleObject(parent, 0) == WAIT_TIMEOUT) TerminateProcess(parent, 0);
    CloseHandle(game);
    CloseHandle(parent);
    return 0;
}

static int RunMain(int argc, wchar_t** argv) {
    if (g_hasConsole) SetConsoleOutputCP(CP_UTF8);

    Options options;
    std::wstring error;
    if (!ParseOptions(argc, argv, options, error)) {
        PrintLine(L"[错误] " + error);
        PrintLine();
        PrintUsage();
        return 2;
    }
    if (options.help) { PrintUsage(); return 0; }

    PrintLine(L"=== Endfield Quick Launch ===");
    PrintLine(std::wstring(L"当前权限: ") + (IsElevated() ? L"管理员" : L"普通用户"));
    PrintLine();

    // ---- 1. 找游戏
    fs::path game = options.game;
    if (game.empty()) {
        const std::wstring configured = ReadConfig(L"game");
        if (!configured.empty()) game = configured;
    }
    if (game.empty()) {
        PrintLine(L"[*] 未指定 --game，正在自动查找 Endfield.exe ...");
        game = FindGameExecutable();
        if (game.empty()) {
            PrintLine(L"[错误] 没有自动找到 Endfield.exe。");
            PrintLine(L"       请用 --game 指定，例如：");
            PrintLine(L"       EndfieldQuickLaunch.exe --game \"D:\\Games\\Endfield\\Endfield.exe\"");
            return 3;
        }
        PrintLine(L"[+] 自动找到: " + game.wstring());
    }
    std::error_code ec;
    if (!fs::is_regular_file(game, ec)) {
        PrintLine(L"[错误] 游戏文件不存在: " + game.wstring());
        return 3;
    }
    if (!IEquals(game.filename().wstring(), L"Endfield.exe")) {
        PrintLine(L"[错误] --game 必须指向 Endfield.exe，当前是: " + game.filename().wstring());
        return 2;
    }
    std::error_code absoluteError;
    game = fs::absolute(game, absoluteError).lexically_normal();
    if (absoluteError) {
        PrintLine(L"[错误] 无法解析游戏路径: " + game.wstring());
        return 3;
    }
    // 诊断/预演不应该留下副作用，只有真要启动时才记住这次的选择。
    if (!options.dryRun && !options.diagnose) WriteConfig(L"game", game.wstring());

    // ---- 2. 找官方启动器
    std::vector<fs::path> launcherCandidates;
    fs::path parentPath;
    if (options.noParent) {
        PrintLine(L"[*] --no-parent：跳过官方启动器父进程（直启模式）");
    } else {
        if (!options.parent.empty()) {
            parentPath = options.parent;
        } else {
            const std::wstring configured = ReadConfig(L"parent");
            if (!configured.empty()) parentPath = configured;
        }

        if (!parentPath.empty()) {
            fs::path resolved;
            if (!ResolveToGamesCore(parentPath, resolved)) {
                PrintLine(L"[错误] 无法从指定路径解析出 Games.exe: " + parentPath.wstring());
                return 6;
            }
            parentPath = resolved;
            PrintLine(L"[+] 使用指定的官方启动器: " + parentPath.wstring());
            const auto trust = VerifyAuthenticode(parentPath);
            if (trust.trusted) {
                PrintLine(L"[+] 签名验证通过" +
                    (trust.signer.empty() ? std::wstring() : L"（" + trust.signer + L"）"));
            } else {
                PrintLine(L"[!] 签名验证未通过 (" + StatusHex(trust.status) + L")");
                if (!options.force) {
                    PrintLine(L"    正式的游戏父进程校验会失败。加 --force 可强行继续。");
                    return 6;
                }
                PrintLine(L"    --force 已指定，继续执行。");
            }
        } else {
            PrintLine(L"[*] 正在扫描本机官方启动器 ...");
            launcherCandidates = CollectLauncherCandidates();
            // 一次验完所有候选：诊断模式全部列出，正常启动时按版本号降序取第一个
            // 签名有效的。最高版本目录可能正在更新（签名暂时无效），这种情况要退到
            // 下一个候选，而不是直接失败。
            std::vector<TrustResult> trusts;
            trusts.reserve(launcherCandidates.size());
            for (const auto& candidate : launcherCandidates)
                trusts.push_back(VerifyAuthenticode(candidate));
            if (options.diagnose) {
                if (launcherCandidates.empty()) PrintLine(L"    （没有找到任何候选）");
                for (size_t index = 0; index < launcherCandidates.size(); ++index) {
                    const TrustResult& candidateTrust = trusts[index];
                    PrintLine(L"    - " + launcherCandidates[index].wstring());
                    PrintLine(std::wstring(L"        签名: ") +
                        (candidateTrust.trusted ? L"有效" : L"无效") +
                        (candidateTrust.signer.empty() ? L"" : L" / " + candidateTrust.signer) +
                        L"  (status=" + StatusHex(candidateTrust.status) + L")");
                }
            }
            size_t chosen = launcherCandidates.size();
            size_t firstPe = launcherCandidates.size();
            for (size_t index = 0; index < launcherCandidates.size(); ++index) {
                if (!IsPeImage(launcherCandidates[index])) continue;
                if (firstPe == launcherCandidates.size()) firstPe = index;
                if (trusts[index].trusted || options.force) { chosen = index; break; }
            }
            if (chosen == launcherCandidates.size()) {
                if (firstPe != launcherCandidates.size()) {
                    PrintLine(L"[!] 所有候选的签名都无效 (" + StatusHex(trusts[firstPe].status) + L")");
                    PrintLine(L"    正式的游戏父进程校验会失败。加 --force 可强行继续。");
                } else if (launcherCandidates.empty()) {
                    PrintLine(L"[错误] 没有找到官方启动器 Games.exe。");
                    PrintLine(L"       请确认已安装鹰角/GRYPHLINK 启动器，或用 --parent 指定，");
                    PrintLine(L"       或加 --no-parent 直接启动（若该版本不做父进程校验）。");
                } else {
                    PrintLine(L"[错误] 找到的候选都不是 PE 映像，可能已损坏。");
                }
                return 6;
            }
            parentPath = launcherCandidates[chosen];
            PrintLine(L"[+] 选中官方启动器: " + parentPath.wstring());
            if (trusts[chosen].trusted) {
                PrintLine(L"[+] 签名验证通过" +
                    (trusts[chosen].signer.empty() ? std::wstring()
                                                  : L"（" + trusts[chosen].signer + L"）"));
            } else {
                PrintLine(L"[!] 签名验证未通过 (" + StatusHex(trusts[chosen].status) + L")");
                PrintLine(L"    --force 已指定，强制继续。");
            }
        }
        if (!options.dryRun && !options.diagnose) WriteConfig(L"parent", parentPath.wstring());
    }

    // ---- 3. 诊断模式
    if (options.diagnose) {
        PrintLine();
        PrintLine(L"--- 诊断结果 ---");
        PrintLine(L"游戏:   " + game.wstring());
        PrintLine(L"父进程: " + (options.noParent ? std::wstring(L"(直启，无父进程)") : parentPath.wstring()));
        PrintLine(L"注入:   " + (options.injectDll.empty() ? std::wstring(L"(无)") : options.injectDll.wstring()));
        PrintLine(L"Steam:  " + std::wstring(options.steam ? L"启用" : L"关闭"));
        return 0;
    }

    // ---- 4. dry-run
    if (options.dryRun) {
        PrintLine();
        PrintLine(L"--- 将要执行 ---");
        PrintLine(L"1. 以 CREATE_SUSPENDED|CREATE_NO_WINDOW|SW_HIDE 启动:");
        PrintLine(L"   " + (options.noParent ? std::wstring(L"(跳过)") : parentPath.wstring()));
        PrintLine(L"2. 以该暂停进程为父进程启动:");
        PrintLine(L"   " + CommandLineFor(game, options.gameArguments));
        PrintLine(L"   工作目录: " + game.parent_path().wstring());
        PrintLine(L"3. 恢复游戏主线程，等待退出，然后清理父进程。");
        return 0;
    }

    // ---- 5. 启动
    // Steam 环境变量必须在创建游戏进程之前设置，否则子进程继承不到。
    if (options.steam) {
        SetEnvironmentVariableW(L"SteamOverlayGameId", L"4732690");
        SetEnvironmentVariableW(L"SteamAppId", L"4732690");
        SetEnvironmentVariableW(L"SteamGameId", L"4732690");
        PrintLine(L"[*] 已设置 Steam 环境变量 (AppID 4732690)");
    }

    OfficialParent parent;
    if (!options.noParent) {
        if (!parent.Start(parentPath, error)) {
            PrintLine(L"[错误] " + error);
            return 4;
        }
        PrintLine(L"[+] 已创建隐藏的官方启动器父进程 (PID " +
            std::to_wstring(GetProcessId(parent.handle())) + L")");
    }

    PROCESS_INFORMATION gameProcess{};
    if (options.noParent) {
        std::wstring command = CommandLineFor(game, options.gameArguments);
        // 注意：lpStartupInfo 不能传 nullptr，否则 CreateProcessW 直接返回
        // ERROR_INVALID_PARAMETER(87)；必须给一个填好 cb 的 STARTUPINFOW。
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                game.parent_path().c_str(), &startup, &gameProcess)) {
            const DWORD lastError = GetLastError();
            PrintLine(L"[错误] " + WinError(L"创建游戏进程失败", lastError));
            if (lastError == ERROR_ELEVATION_REQUIRED)
                PrintLine(L"       游戏要求管理员权限，请以管理员身份运行本程序重试。");
            return 4;
        }
    } else {
        // 控制台子系统程序在"父进程属性"下会以 0xC0000142 立即退出（拿不到控制台），
        // 因此为 CUI 目标显式分配新控制台；游戏本体是 GUI 子系统，不受影响。
        DWORD creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
        if (PeSubsystem(game) == IMAGE_SUBSYSTEM_WINDOWS_CUI)
            creationFlags |= CREATE_NEW_CONSOLE;
        if (!parent.CreateGame(game, options.gameArguments,
                creationFlags, gameProcess, error)) {
            PrintLine(L"[错误] " + error);
            PrintLine(L"       若提示拒绝访问/需要提升，请以管理员身份运行本程序重试。");
            return 4;
        }
    }
    PrintLine(L"[+] 已创建游戏进程 (PID " + std::to_wstring(gameProcess.dwProcessId) + L")");

    if (ResumeThread(gameProcess.hThread) == static_cast<DWORD>(-1)) {
        PrintLine(L"[错误] " + WinError(L"恢复游戏主线程失败", GetLastError()));
        TerminateProcess(gameProcess.hProcess, 0);
        CloseHandle(gameProcess.hThread);
        CloseHandle(gameProcess.hProcess);
        return 5;
    }
    CloseHandle(gameProcess.hThread);
    PrintLine(L"[+] 游戏已启动");

    // ---- 6. 可选注入
    if (!options.injectDll.empty()) {
        std::error_code injectError;
        const fs::path injectPath = fs::absolute(options.injectDll, injectError);
        if (injectError || !fs::is_regular_file(injectPath, injectError)) {
            PrintLine(L"[!] 注入目标不可用，跳过: " + options.injectDll.wstring());
        } else {
            if (options.injectTimeoutMs > 0) {
                PrintLine(L"[*] 等待 GameAssembly.dll 就绪后注入 ...");
                if (!WaitForRemoteModule(gameProcess.dwProcessId, L"GameAssembly.dll",
                        options.injectTimeoutMs))
                    PrintLine(L"[!] 等待 GameAssembly.dll 超时，仍然尝试注入。");
                // 模块加载完到能安全 LoadLibraryW 之间还有一小段托管运行时初始化。
                Sleep(1500);
            }
            if (InjectLibrary(gameProcess.hProcess, gameProcess.dwProcessId, injectPath, error))
                PrintLine(L"[+] 注入完成: " + injectPath.filename().wstring());
            else
                PrintLine(L"[!] 注入失败: " + error);
        }
    }

    if (options.noWait) {
        PrintLine(L"[*] --no-wait：不等待游戏退出。");
        if (!options.noParent && parent.valid()) {
            // 一定不能在这里 TerminateProcess 父进程（否则游戏后续校验会 10100）。
            if (SpawnKeeper(GetProcessId(parent.handle()), gameProcess.dwProcessId))
                PrintLine(L"[+] 已启动看护进程，游戏退出后自动清理隐藏父进程");
            else
                PrintLine(L"[!] 看护进程创建失败，隐藏父进程可能残留到重启");
            parent.Detach();
        }
        CloseHandle(gameProcess.hProcess);
        return 0;
    }

    PrintLine(L"[*] 等待游戏退出 ...");
    WaitForSingleObject(gameProcess.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(gameProcess.hProcess, &exitCode);
    CloseHandle(gameProcess.hProcess);
    PrintLine(L"[+] 游戏已退出，退出码 " + std::to_wstring(exitCode));

    if (!options.noParent) {
        parent.Cleanup();
        PrintLine(L"[+] 已清理隐藏的官方启动器父进程");
    }
    return 0;
}

// ------------------------------------------------------------------ 程序入口
//
// 以 GUI 子系统链接（-mwindows），双击运行时不会闪出黑色控制台窗口。
// 清单文件里声明了 requireAdministrator，双击/从资源管理器运行时由 Windows
// 直接弹出 UAC 提权对话框（游戏与官方启动器本身也是 requireAdministrator）。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    InitOutput();

    // 隐藏的看护模式：--keeper <父进程PID> <游戏PID>，完全静默，不弹任何窗口。
    if (argv && argc == 4 && std::wstring(argv[1]) == L"--keeper") {
        const int code = RunKeeper(static_cast<DWORD>(_wtoi(argv[2])),
            static_cast<DWORD>(_wtoi(argv[3])));
        LocalFree(argv);
        return code;
    }

    int code = 1;
    if (!argv) {
        PrintLine(L"[错误] 无法解析命令行。");
        code = 2;
    } else {
        try {
            code = RunMain(argc, argv);
        } catch (...) {
            // 以 GUI 子系统链接时未捕获异常会静默消失，这里兜住并给出可诊断的信息。
            PrintLine(L"[错误] 程序遇到未预期的异常，已中止。");
            code = 1;
        }
        LocalFree(argv);
    }
    return Finish(code);
}
