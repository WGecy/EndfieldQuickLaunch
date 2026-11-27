# EndfieldQuickLaunch

直接启动《明日方舟：终末地》(Arknights: Endfield)，**跳过官方启动器界面**。

游戏（`OneChannelLC.dll` / U8SDK）启动时会校验"我是不是被官方启动器拉起来的"，校验链条是
**父进程映像名 → 父进程文件的 Authenticode 签名 → 签名者证书指纹**。所以自己造一个同名
`Games.exe` 没有任何用。

本项目不伪造、不修改、不复制官方启动器，而是**借用真身**：

> 用 `CREATE_SUSPENDED` 把正版 `Games.exe` 以隐藏窗口拉起来，主线程永不 `ResumeThread`；
> 再用 `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` 让 `Endfield.exe` 的直接父进程变成这个挂起的
> `Games.exe`。对游戏来说，父进程名字对、文件签名有效、证书指纹匹配 —— 于是校验通过。
> 因为主线程从未运行，官方启动器的窗口一行代码都没执行过。

思路与 [Better-Endfield](https://github.com/Dr-hydra/Better-Endfield) 的
`native/loaders/shared/official_launcher.cpp` 一致，本仓库是一个**独立、零依赖、单文件**的
最小实现（约 1350 行 C++20，不注入任何代码，不含 Better-Endfield 的任何模块；
Release 构建默认运行后不在系统里留下任何文件）。

---

## 工作流程

```
你双击 EndfieldQuickLaunch.exe（UAC 提权）
  │
  ├─(1) 扫描注册表 / 卸载表 / Program Files 找到官方 Games.exe，验签
  │
  ├─(2) CreateProcess(Games.exe, CREATE_SUSPENDED | CREATE_NO_WINDOW, SW_HIDE)
  │        └─> 进程活着、签名有效、界面永不出现（主线程不 Resume）
  │
  ├─(3) UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, parentPid)
  │      CreateProcess(Endfield.exe, CREATE_SUSPENDED)
  │        └─> 游戏的直接父进程 = 步骤 (2) 那个挂起的 Games.exe
  │
  ├─(4) ResumeThread ──> 游戏正常启动、正常登录
  │
  └─(5) 等游戏退出 ──> TerminateProcess 清理自建的隐藏父进程，不留僵尸
```

游戏侧的校验（逆向记录见 Better-Endfield `docs/host/research/1.6.3/launcher-check/LAUNCHER_CHECK.md`）：

| 步骤 | 检查内容 | 失败码 |
|---|---|---|
| 1 | 父进程映像名与 `parentNames` 精确比对（如 `Games.exe`） | `10100` |
| 2 | 对父进程文件做 `WinVerifyTrust`（`WINTRUST_ACTION_GENERIC_VERIFY_V2`） | `10101` |
| 3 | 签名者证书 SHA-1 / SHA-256 与白名单 `fingerprints` 比对 | `10102` |

三个失败码分别在"名字不对 / 没签名 / 签名者不对"时出现 —— 这就是为什么必须借真身。

## 编译

需要 **MinGW-w64**（g++ 支持 C++20，且带 `windres` / binutils）。没有 MSVC 依赖，不需要 CMake。

```cmd
build.bat
```

或手动：

```cmd
windres -I res -o manifest_res.o res\resources.rc
g++ -std=c++20 -O2 -municode -mwindows -DUNICODE -D_UNICODE ^
    -Wall -Wextra -Wpedantic -Wshadow ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    -static -static-libgcc -static-libstdc++ ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections -s ^
    -o EndfieldQuickLaunch.exe src\main.cpp manifest_res.o ^
    -lwintrust -lcrypt32 -lshell32 -ladvapi32 -lversion
```

两个产物是**各自自包含的单个 exe**（静态链接 libstdc++ / libgcc / winpthread），只依赖系统自带的
`KERNEL32` / `ADVAPI32` / `CRYPT32` / `WINTRUST` / `SHELL32`，加上 Windows 10/11 必有的 UCRT。

`build.bat` 一次编译出两个版本：

| 产物 | 构建方式 | 行为 |
|---|---|---|
| `EndfieldQuickLaunch.exe` | 默认 | **Release：默认不写盘** —— 不写日志、不写配置，运行后系统里不留任何文件；只有加了 `--config` 才在 exe 同目录读写 `config.ini` |
| `EndfieldQuickLaunch-debug.exe` | 额外加 `-DEQL_DEBUG` | **Debug：写 `last-run.log`**，配置默认缓存在 `%LOCALAPPDATA%`；同样可以用 `--config` 换成便携模式 |

日志是编译期的差别：`#if EQL_DISK` 把写日志那整段代码排除在 Release 之外（不是运行期开关），
所以 Release 里连 `last-run.log` 这个字符串都不存在。

配置缓存的位置则由运行参数 `--config` 决定，两个构建行为一致（见下面的「便携模式」）。
不加 `--config` 时，Release 不缓存任何东西、每次启动都要重新自动扫描（本机实测 2–3 秒），
Debug 则用 `%LOCALAPPDATA%` 下的缓存，下次直接命中。

GitHub Release 里两个 exe 都会附上；日常用 Release 那个，遇到问题再换 Debug 那个跑一遍看日志。

两个关键编译选项：

- `-mwindows` —— GUI 子系统，双击**不会闪出黑色控制台窗口**，程序全程不弹任何对话框。
- `res/resources.rc` + `res/EndfieldQuickLaunch.manifest` —— 把 `requireAdministrator` 清单
  编进 exe，**Windows 启动时直接弹 UAC**（游戏和官方启动器本身都是 `requireAdministrator`，
  父进程必须同提权级别才能创建成功）。

## 用法

```cmd
:: 最简单：自动找游戏 + 自动找官方启动器
EndfieldQuickLaunch.exe

:: 游戏装在奇怪的位置
EndfieldQuickLaunch.exe --game "D:\Games\Endfield\Endfield.exe"

:: 有多个启动器版本，指定一个
EndfieldQuickLaunch.exe --parent "C:\Program Files\Hypergryph Launcher\1.6.0\Games.exe"

:: 先看看会做什么，不启动
EndfieldQuickLaunch.exe --dry-run

:: 只诊断：列出找到的启动器、签名状态、游戏路径
EndfieldQuickLaunch.exe --diagnose

:: 顺便注入一个 DLL（例如 Better-Endfield 的 Bootstrap）
EndfieldQuickLaunch.exe --inject "C:\BE\loaders\BetterEndfieldNext.Bootstrap.dll"

:: `--` 之后的参数原样传给游戏
EndfieldQuickLaunch.exe -- -force-d3d11
```

| 参数 | 说明 |
|---|---|
| `--game <路径>` | `Endfield.exe` 路径，缺省自动查找 |
| `--parent <路径>` | `Games.exe` 路径，缺省自动扫描 |
| `--no-parent` / `--direct-launch` | 不伪造父进程直接启动（只对不做校验的版本有效） |
| `--inject <dll>` | 启动后等 `GameAssembly.dll` 就绪，再远程线程 `LoadLibraryW` 注入 |
| `--inject-timeout <毫秒>` | 等待 `GameAssembly.dll` 的上限，默认 `90000`，有效范围 `0`–`3600000`；`0` = 不等待立即注入 |
| `--config` | 读写 **exe 同目录**的 `config.ini`，把游戏/启动器路径缓存下来（两个构建都支持） |
| `--steam` | 设置 `SteamOverlayGameId` / `SteamAppId` / `SteamGameId` = `4732690` |
| `--diagnose` | 只诊断，不启动，也不写配置 |
| `--dry-run` | 打印将要执行的命令行，不启动，也不写配置 |
| `--no-wait` | 启动后立即返回（另起 `--keeper` 看护进程，游戏退出后再清理隐藏父进程） |
| `--force` | 签名验证失败也继续 |
| `-h` / `--help` | 帮助 |

不加 `--config` 时：Debug 构建把游戏路径与启动器路径缓存到
`%LOCALAPPDATA%\EndfieldQuickLaunch\config.ini`，Release 构建不保存任何东西，每次都重新自动查找。

### 便携模式（`--config`）

加上 `--config`，两个构建都改成读写 **exe 同目录**的 `config.ini`：

```cmd
:: 第一次：指定游戏路径，顺手写进 exe 旁边的 config.ini
EndfieldQuickLaunch.exe --config --game "D:\Games\Endfield\Endfield.exe"

:: 之后：直接跑，路径从 config.ini 里读
EndfieldQuickLaunch.exe --config
```

`config.ini` 不存在时会自动新建（父目录也会一并创建）；`--config` 只影响缓存位置，
不会把 Release 变成"偷偷写 `%LOCALAPPDATA%`"。把 exe 和 `config.ini` 一起拷到别处仍然可用，
这是把启动器塞进随身盘或固定目录的用法。

注意 `--config` 是**每次都要带上**的：它不是一个记忆开关，程序不会把"要用便携配置"这件事记在别处。

### 双击使用 / 输出去哪了

exe 清单写的是 `requireAdministrator`，**双击就会弹 UAC**，点"是"即可，不需要右键"以管理员身份运行"。
程序是 GUI 子系统，**不显示控制台、不弹任何提示框**。

- 从 cmd / PowerShell（管理员）运行时，两个版本都正常打到控制台。
- 双击运行时没有窗口：**Release 什么都不留**（除非带了 `--config`，那会更新 exe 旁边的
  `config.ini`）；**Debug** 把本次输出写到
  `%LOCALAPPDATA%\EndfieldQuickLaunch\last-run.log`（UTF-8 带 BOM，记事本直接可读）。
- 想只看诊断不启动游戏：给快捷方式加 `--diagnose`，或 `EndfieldQuickLaunch.exe --diagnose > diag.txt`。

## 自动查找逻辑

**官方启动器**（按优先级）：

1. `HKEY_CLASSES_ROOT\hypergryphlauncher*\shell\open\command` 与 `gryphlinklauncher*` 协议注册项
2. 卸载表（HKCU/HKLM × 64/32 位）里 `DisplayName` 为 *鹰角启动器 / Hypergryph Launcher /
   GRYPHLINK / Gryphlink / Gryphline Launcher* 的 `InstallLocation`
3. `%ProgramFiles%\Hypergryph Launcher`、`%ProgramFiles(x86)%\...`、`%LOCALAPPDATA%\GRYPHLINK` 等
4. 在以上目录及其**带版本号的子目录**里找 `Games.exe` / `Launcher.exe`，按版本号从高到低，
   取第一个**签名有效且是 PE 映像**的候选（最高版本目录正在更新、签名暂时无效时会自动退到下一个）

**游戏**（按优先级）：

1. 配置文件里保存的路径
2. 卸载表中名字含 `Endfield` / `终末地` 的条目，在其安装目录往下最多三层找 `Endfield.exe`
3. 从**非系统盘根**（`D:\`、`E:\` …）开始扫，再扫 `%ProgramFiles%`、
   `%ProgramFiles(x86)%`、`%LOCALAPPDATA%`、`%SystemDrive%\`，深度 3 的有界扫描

扫描细节：

- 每个起点单独给 2 万次目录项的预算，再加 12 万次的总上限：单个目录再大也不会把后面的起点饿死。
  这一点很关键——**提权**运行时 `C:\Program Files` 下那些普通权限看不到的条目
  （`WindowsApps` 里上百个 Appx 包等）都能枚举，旧的"全局共用 4 万"预算会在扫到 C: 盘时就用完，
  于是不带缓存启动会误报"没有自动找到 Endfield.exe"。
- 非系统盘排在系统盘前面，游戏基本不装在 C:。
- 跳过 junction / 符号链接（如 `C:\Users\All Users`），既不重复遍历同一棵树，也不会因成环耗尽预算。
- `Windows`、`WindowsApps`、`SystemApps`、`WinSxS`、`ProgramData`、`$Recycle.Bin` 等目录直接不进。
- 最多收集 8 个候选；命中多个时取最近修改过的那份。

## 自动查找 + 校验的细节

- 候选 `Games.exe` 会走一遍 `WinVerifyTrust`，并用 `CryptQueryObject` + `CertGetNameStringW`
  读出签名者名字，打印在 `--diagnose` 里。
- 构建父进程时优先调用 `kernelbase!CreateProcessW`：先读它自己的首字节，若不是
  `0xE9` / `0xEB` / `0xFF25`（没被 Steam overlay 之类内联 hook）就直连该实现，
  跳过 `kernel32` 上的转发函数；若连 kernelbase 都被改写了，则退回正常导出。
- `PeSubsystem()` 读 PE 头判断目标子系统：GUI 目标（`Endfield.exe`）不需要额外标志；
  控制台目标在伪造父进程下会因继承不到控制台而 `0xC0000142` 秒退，故补 `CREATE_NEW_CONSOLE`。

## 真机验证（2026-11-24）

本机正版环境实测通过：官方启动器 `C:\Program Files\Hypergryph Launcher`（`1.6.0\Games.exe`），
游戏 `D:\Games\Arknights Endfield\Endfield.exe`，以管理员权限运行。

进程树（实际观测）：

```
EndfieldQuickLaunch.exe 21720
  └─ Games.exe 21140          <- 隐藏、挂起、签名有效的官方父进程
       └─ Endfield.exe 4772   <- 直接父进程 = 上面那个
            └─ PlatformProcess.exe 14948
```

结果：

- `OneChannelLC.log` 记录 `Parent process check success:21140` /
  `Record check result:{"code":2003,"msg":{"code":0,"msg":{"filePath":"c:\\program files\\hypergryph launcher\\1.6.0\\games.exe"}}}`
  —— 名称 + 签名 + 证书指纹三项全部通过。
- `u8sdk_hg_pc.log` 登录回调成功（`code:2000` → `LoginCallback` → `code:2002` → `code:30`）。
- 游戏渲染到标题画面「明日方舟 终末地 / ARKNIGHTS ENDFIELD」，版本号 `1.6.3.C10781849_E10704668`。
- 稳定运行 383 秒后正常退出（退出码 0），无 `10100` / `10101` / `10102`，无崩溃。

**关键教训**：早期版本在 `--no-wait` 下让隐藏父进程随本程序退出，游戏随后补做父进程校验时找不到
父进程，记录 `Not found matched name` 并返回 `10100`。现在 `--no-wait` 会先 `SpawnKeeper()`
起一个分离的看护进程（内部参数 `--keeper <父PID> <游戏PID>`，请勿手动使用），由它等游戏退出后
再 `TerminateProcess` 清理父进程。

## 自校验（不需要游戏）

`test/selfcheck.cpp` 是一个独立的探针程序，用来证明"父进程伪造"这条链路真的成立：
它自己扮成"假父进程"（挂起）和"假子进程"，让子进程用 `Toolhelp32` 反查自己的真实父 PID 并写文件，
最后比对是否等于期望值。**全程不启动游戏、不写入游戏目录、不需要管理员权限。**

```cmd
cd test
build.bat
selfcheck.exe
```

典型输出：

```
[1/3] 以 CREATE_SUSPENDED 启动假父进程 ...
      PID = 8744（保持挂起）
[2/3] 用 PROC_THREAD_ATTRIBUTE_PARENT_PROCESS 启动假子进程 ...
      PID = 7448
[3/3] 等子进程自证父进程 ...

子进程自报：
pid=7448
parent=8744
expected=8744
match=1

结果：PASS —— 子进程的直接父进程就是那个隐藏挂起的进程。
```

## 已知限制

- **必须保留正版官方启动器**。程序只是"借用"它的进程，不修改、不复制它。
- 不修改游戏文件、不写游戏目录（除非 `--inject` 且你指定了游戏目录下的 DLL）。
- 游戏退出后会 `TerminateProcess` 清理自建的隐藏父进程，不留僵尸。
- 若游戏提示"请通过官方启动器打开游戏"（`10100`），说明当前版本加强了校验，或用了签名无效的父进程。
- 本质是**父进程 PID 伪造**。游戏侧一旦改为校验 IPC 握手、句柄继承、会话关系或启动令牌，
  本方法即失效 —— 这不是 bug，是原理边界。
- `--inject` 要求目标进程与本程序位数一致；本程序为 x64，32 位游戏无法注入，会明确报错。
- 自动查找命中**多个**游戏时取最近修改过的那份；也可用 `--game` 显式指定。
- `--no-wait` 的看护进程按 PID 等待游戏退出；极端情况下若有 PID 复用，会多等一段时间。
- `--config` 要往 exe 所在目录写 `config.ini`。放在 `Program Files` 这类普通用户无写权限的位置时，
  因为程序本身就以管理员运行通常没问题；若写入失败会打印 `[!] 配置写入失败`，其余功能不受影响。
- 只在 Windows 10/11 x64 上测试过。

## 退出码

| 退出码 | 含义 |
|---|---|
| `0` | 成功（`--help` / `--dry-run` / `--diagnose` 正常结束也是 0） |
| `1` | 未预期的异常 |
| `2` | 命令行参数错误 |
| `3` | 找不到游戏，或路径无法解析 |
| `4` | 创建游戏进程失败 |
| `5` | `ResumeThread` 失败 |
| `6` | 官方启动器相关失败（找不到 / 签名无效 / 创建父进程失败） |

## 免责声明

仅供学习 Windows 进程/启动器工作原理使用。你需要自己拥有该游戏与官方启动器的合法副本。
本项目不提供游戏本体、不绕过任何付费或联网验证、不修改游戏程序。使用风险自负。

## 致谢

- [Better-Endfield](https://github.com/Dr-hydra/Better-Endfield) —— 原始思路与逆向记录来源。

## License

MIT
