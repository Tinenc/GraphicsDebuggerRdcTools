# NarakaMobile (`F:\NarakaMobile`) 注入方案

> 分支：`tinecmatool/naraka-inject`（基于 `tinecmatool/manualmap-inject`）
> 目标真进程：`F:\NarakaMobile\game\NarakaBladepointMobile.exe`
> 反作弊：网易易盾 NEAC（`NeacSafe64.sys` / `YJNeacClient.exe` / `NeacInterface.dll`）+ CrashHunter

本分支把三条路径合在一起，按推荐顺序尝试：

| # | 策略 | 实现位置 | 默认状态 |
|---|---|---|---|
| 1 | 子进程黑名单 + 可选白名单/路径前缀 | `sys_win32_hooks.cpp` `ShouldInject` | **已启用** |
| 2 | Thread-hijack → CreateRemoteThread（manual-map 可选） | `win32_process.cpp` | **hijack 开 / manual-map 关** |
| 3 | 磁盘 PE Import Table 注入 | `util/pe_import_inject/` + `naraka_patch.ps1` | **按需手动跑** |

---

## 0. 进程树（必须搞清楚再动手）

```
NarakaMobileLauncher.exe          <- Electron NGP 启动器（不要注入）
  +- StartGame_l22.exe           <- 中间启动器（不要注入）
       +- NarakaBladepointMobile.exe  <- * Unity IL2CPP 真游戏（要注入）
YJNeacClient.exe                 <- NEAC 客户端（绝不能注入）
UnityCrashHandler64.exe / ...    <- 崩溃/直播/webview 辅助（不要注入）
```

图形栈：UnityPlayer + GameAssembly（IL2CPP），通常 D3D11/12。

---

## 1. 黑名单 / 白名单 / 路径前缀

`ShouldInject` 在 hook 子进程 `CreateProcess*` 时决定要不要跟注入。

### 1.1 硬编码黑名单（始终生效）

`tinecmatoolcmd.exe` / `qtinecmatool.exe` / `platformprocess.exe`（鸣潮）以及：

`narakamobilelauncher.exe`、`yjneacclient.exe`、
`unitycrashhandler64.exe`、`unicrashreporter.exe`、`narakam_patcher.exe`、
`narakam_updater.exe`、`ffmpeg.exe`、`ccmini.exe`、直播/webview 辅助、
updater/elevate/uninst 等。

**不要**把 `startgame_l22.exe` 放进黑名单：登录器链路是
`Launcher → StartGame_l22 → NarakaBladepointMobile`，必须允许注入
StartGame，才能把 hook 传到真游戏；否则会出现 Connected 但 **API: None**。

`webviewsupport\...\render.exe` 仅在路径含 `webviewsupport` 时才跳过。

### 1.2 推荐环境变量（Global Hook / 经启动器抓帧时）

在启动 `qTinecmaTool.exe` **之前**设置：

```powershell
$env:TINECMATOOL_CHILD_WHITELIST = "narakabladepointmobile.exe;startgame_l22.exe"
$env:TINECMATOOL_CHILD_PATH_PREFIX = "f:\narakamobile\game"
```

直接 Launch Application 指向 `NarakaBladepointMobile.exe` 时，不经过 `ShouldInject`，这两项可省略。

---

## 2. 推荐抓帧流程（策略 1+2）

1. 编译本分支（VS2022 -> `renderdoc.sln` -> x64 / Development）
2. **优先**：File -> Launch Application
   - Executable: `F:\NarakaMobile\game\NarakaBladepointMobile.exe`
   - Working Dir: `F:\NarakaMobile\game`
3. 若必须经启动器登录：开 Global Hook + 上面两个 env，再开
   `F:\NarakaMobile\NGP\NarakaMobileLauncher.exe`
4. 默认注入链路：
   1. SetThreadContext + LoadLibraryW
   2. 失败 -> CreateRemoteThread
   3. Manual-map **默认关**（见 §4）

| 环境变量 / 宏 | 作用 |
|---|---|
| `TINECMATOOL_DISABLE_THREADHIJACK=1` | 跳过线程劫持，直接 CRT |
| `TINECMATOOL_ENABLE_MANUALMAP=1` | 运行时启用 manual-map（默认关；当前 smoke 会 FAIL） |
| `TINECMATOOL_DISABLE_MANUALMAP=1` | 强制关 manual-map |

---

## 3. 策略 3：磁盘 PE Import 注入（最后手段）

```powershell
cd "C:\Program Files\GraphicsDebuggerRdcTools\util\pe_import_inject"
powershell -ExecutionPolicy Bypass -File .\naraka_patch.ps1 -Action probe
powershell -ExecutionPolicy Bypass -File .\naraka_patch.ps1 -Action patch `
    -InjectDll "C:\path\to\TinecmaTool.dll"
powershell -ExecutionPolicy Bypass -File .\naraka_patch.ps1 -Action restore
```

风险：本地完整性校验；`NeacSafe64.sys` 内核扫描。`launcher_patcher/` 原为鸣潮设计，Naraka Electron 启动器未必通用。

---

## 4. 自验证结果（本机迭代）

```powershell
powershell -ExecutionPolicy Bypass -File util\pe_import_inject\naraka_verify.ps1 -RunPeSanity
```

| 检查 | 结果 |
|---|---|
| 黑名单匹配（游戏允许 / Launcher·NEAC·StartGame 拦截） | PASS |
| 白名单+路径前缀 | PASS |
| `F:\NarakaMobile` 关键文件存在 | PASS |
| PE Import sanity（hello.exe + PoC DLL） | PASS |
| `TinecmaTool.dll` / `TinecmaToolcmd.exe` 编译 | PASS |
| `capture` hello.exe + **thread-hijack only** | PASS（~0.3s） |
| `capture` hello.exe + **CRT only** | PASS |
| `capture` hello.exe + **manual-map only** | **FAIL**（故默认关闭） |
| `inject --PID` 进已运行 notepad | FAIL/不稳（请用 Launch Application） |

结论：对 `F:\NarakaMobile` **先用 Launch Application 指向真游戏 + 默认 thread-hijack**；Global Hook 时设白名单；PE patch 作最后手段；manual-map 待修后再开。

---

## 5. 已知限制

- 内核驱动 `NeacSafe64.sys` 不在本仓库对抗范围内。
- 不要对已运行空闲 GUI 做 `inject --PID` 冒烟；请用 Launch Application。
- Manual-map 代码仍在树内，默认不走；修好后再把 `TINECMATOOL_USE_MANUALMAP_INJECT` 调回 1。
