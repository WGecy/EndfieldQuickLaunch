# 免启动器启动的原理笔记

这份文档记录"为什么可以不用官方启动器界面就把游戏拉起来"的完整推理链，以及所有结论的出处。

## 1. 游戏在防什么

《明日方舟：终末地》的 PC 端启动链路是：

```
Hypergryph Launcher (Games.exe)
    └─ Endfield.exe            <- Unity 本体
         └─ PlatformProcess.exe
```

`Endfield.exe` 加载的 `OneChannelLC.dll`（渠道 SDK 层）在进程启动早期会做一次
"启动来源校验"。校验不通过就弹窗提示"请通过官方启动器打开游戏"并退出。

校验分三步，全部通过才放行：

| 步骤 | 具体行为 | 失败返回码 |
|---|---|---|
| 1 | 用 `NtQueryInformationProcess` / 工具帮助库取父进程，比较**映像文件名**（basename）与内置的 `parentNames` 列表，通常就是 `Games.exe` | `10100` |
| 2 | 用 `WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2)` 校验**父进程可执行文件**的 Authenticode 签名 | `10101` |
| 3 | 取签名者证书的 SHA-1 / SHA-256 指纹，与内置的 `fingerprints` 白名单比对 | `10102` |

第 3 步的指纹属于上海鹰角网络（`Shanghai Hypergryph Network Technology Co., Ltd.`）。

**推论**：自己写一个叫 `Games.exe` 的程序是没用的 —— 名字能过第 1 步，第 2、3 步必然失败。
必须让**真实的、签名有效的官方 `Games.exe`** 成为父进程。

## 2. 怎么"借"到那个父进程

Windows 允许创建进程时指定**"父进程"* —— 但只是记一个 PID，不要求调用者是它，也不要求它
正在运行用户代码：

```c
// 1. 把真身拉起来，但一个字节都不执行
CreateProcessW(gamesPath, ...,
    NULL, NULL, FALSE,
    CREATE_SUSPENDED | CREATE_NO_WINDOW,  // 挂起 + 不建窗口
    NULL, gamesDir,
    &si /* SW_HIDE */, &pi);
//    <<< 关键：这里永远不调用 ResumeThread(pi.hThread)

// 2. 让游戏认它做爹
SIZE_T size = 0;
InitializeProcThreadAttributeList(NULL, 1, 0, &size);
LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(size);
InitializeProcThreadAttributeList(attrs, 1, 0, &size);
UpdateProcThreadAttribute(attrs, 0,
    PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &pi.hProcess, sizeof(HANDLE),
    NULL, NULL);

CreateProcessW(gamePath, ...,
    NULL, NULL, FALSE,
    EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
    NULL, gameDir,
    &siEx.StartupInfo, &gamePi);
ResumeThread(gamePi.hThread);   // 游戏真正开跑
```

于是：

- 父进程 **PID** 指向那个挂起的 `Games.exe` → 第 1 步"映像名 = Games.exe"通过；
- 父进程**文件**就是正版 `C:\Program Files\Hypergryph Launcher\<ver>\Games.exe` → 第 2 步验签通过；
- 签名者是鹰角 → 第 3 步指纹比对通过。

而因为主线程一直挂着，`Games.exe` 的入口点（`WinMain`）从未执行 —— 它不会去创建窗口、
不会去连服务器、不会去更新、不会去启动第二个游戏。它只是"存在"。

## 3. 三个必须注意的技术细节

### 3.1 `CreateProcessW` 不能被 Steam overlay hook 掉

Steam overlay（`gameoverlayrenderer64.dll`）会内联 hook `kernel32!CreateProcessW`。
被 hook 的版本在 `CREATE_SUSPENDED` 场景下行为不完全可靠。规避办法是直接调
`kernelbase!CreateProcessW`（NTDLL 之上、kernel32 之下的真正实现）：

```c
HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
auto fn = (decltype(&CreateProcessW))GetProcAddress(kb, "CreateProcessW");
```

更稳的做法是先检查 `kernelbase!CreateProcessW` 首字节是不是跳转指令
（`0xE9` rel32 / `0xEB` rel8 / `0xFF 0x25` `[rip+disp]`）：

- **没被 hook** → 直接调 kernelbase 的实现（跳过 kernel32 上的转发函数，hook 也就被绕过了）；
- **被 hook 了** → kernelbase 本身已不可信，退回走 kernel32 的正常导出。

Better-Endfield 的 `CreateProcessWithoutOverlayHook` 就是这么做的，本项目的实现与其一致。

### 3.2 CUI 子进程会以 `0xC0000142` 秒退

如果目标程序是**控制台子系统**，在伪造父进程下它会尝试继承父进程的控制台 ——
但那个父进程是 `CREATE_NO_WINDOW` 且挂起的，没有可继承的控制台，CRT 初始化阶段就死了，
退出码 `0xC0000142`（`STATUS_DLL_INIT_FAILED`）。

解决办法：给 CUI 目标补 `CREATE_NEW_CONSOLE`。`Endfield.exe` 是 **GUI 子系统**
（PE `IMAGE_OPTIONAL_HEADER.Subsystem == 2`），不受影响；本项目用 `PeSubsystem()`
读 PE 头自动判断。

### 3.3 `lpStartupInfo` 不能是 `nullptr`

`CreateProcessW(..., NULL /*lpStartupInfo*/, ...)` 会直接返回
`ERROR_INVALID_PARAMETER`（87）。必须传一个填好 `cb = sizeof(STARTUPINFOW)` 的结构体。

### 3.4 提权级别必须一致

`Endfield.exe` 与官方 `Games.exe` 的清单里都是 `requestedExecutionLevel level="requireAdministrator"`。
如果本程序以普通用户权限运行，用普通权限去 `CreateProcess` 一个 `requireAdministrator` 的
exe 会失败（`ERROR_ELEVATION_REQUIRED`, 740）。

本项目把 `requireAdministrator` 写进自己的清单 —— 双击即弹 UAC，天然与目标同级。

## 4. 清理

游戏退出后，那个挂起的 `Games.exe` 还占着一个进程（约 20 MB 内存）。

- 默认模式（前台等待）：本程序 `WaitForSingleObject(游戏进程)`，游戏一退立刻
  `TerminateProcess(父进程)`。
- `--no-wait` 模式：本程序要立刻返回，不能自己等。于是 `SpawnKeeper()` 另起一个
  分离的自身副本（内部参数 `--keeper <父PID> <游戏PID>`），由它等游戏退出再清理。

`--no-wait` 下**绝不能**让父进程随本程序一起退出：游戏会在启动后某个时刻补做一次
父进程校验，这时父进程已经没了，`OpenProcess` 失败 → 记录 `Not found matched name`
→ 返回 `10100`。这是早期版本踩过的坑。

## 5. 出处

- Better-Endfield 仓库：<https://github.com/Dr-hydra/Better-Endfield>
  - `native/loaders/shared/official_launcher.cpp`
    - `InstalledRoots()` —— 注册表 / 卸载表 / Program Files 三路查找
    - `ResolveToGamesCore()` —— 按版本号选最高的 `Games.exe`
    - `OfficialParent::Start()` —— 挂起启动 + 永不 Resume
    - `OfficialParent::CreateGame()` —— `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`
  - `docs/host/research/1.6.3/launcher-check/LAUNCHER_CHECK.md` —— 三步校验的逆向记录
- Microsoft Learn：`UpdateProcThreadAttribute` / `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`、
  `WinVerifyTrust`。
