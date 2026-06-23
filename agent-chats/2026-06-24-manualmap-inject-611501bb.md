# 2026-06-24 · manual-map DLL 注入会话摘要

- 原始 transcript：`agent-chats/2026-06-24-manualmap-inject-611501bb.jsonl`（≈196 KB）
- 提交分支：`tinecmatool/manualmap-inject`（origin: `github.com/Tinenc/GraphicsDebuggerRdcTools`）
- 工作树：`C:\Program Files\GraphicsDebuggerRdcTools-threadctx-inject`（与主仓库 `C:\Program Files\GraphicsDebuggerRdcTools` 是 sibling clone，互不影响）
- 配套文档：[`TINECMATOOL_MANUALMAP_INJECT.md`](../TINECMATOOL_MANUALMAP_INJECT.md)

## 1. 需求 & 关键决策

| 决策点 | 选择 | 备注 |
|---|---|---|
| 在哪里建工作目录 | sibling clone (`...-threadctx-inject` 后缀) | 不污染主工作树 |
| 起点分支 | `tinecmatool/threadctx-inject` | 已有 SetThreadContext 劫持基线 |
| 注入方案 | manual map（PE 手动映射） | option 3：完全不调 `LoadLibrary` |
| 修改面 | `only_dll`：仅 `InjectDLL` 这一路径 | 不动 `InjectFunctionCall` 等下游 |
| 回退策略 | `chain`：manual map → thread-hijack → CreateRemoteThread | 三级降级 + 日志 |
| 提交方式 | `new_branch` = `tinecmatool/manualmap-inject` | 不动 threadctx-inject 基线 |
| 实现完整度 | `full`：reloc / IAT / TLS / SEH / TLS callbacks / EntryPoint 全做 | |

## 2. 改动文件

| 文件 | 性质 | 关键内容 |
|---|---|---|
| `renderdoc/os/win32/win32_process.cpp` | 修改 | 新增 `InjectDLL_ManualMap` 及 14 个 `MM_*` / `ThreadHijackCall` / `BuildCallShellcode` 辅助函数；`InjectDLL` 签名从 `void` 改为 `uintptr_t`；`StartGlobalHook` 使用新返回值 |
| `TINECMATOOL_MANUALMAP_INJECT.md` | 新增 | 设计 / 编译宏 / 流程 / 限制 / `AppInit_DLLs` 处理 / 调试 |

## 3. 编译 / 运行验证

- VS2022 + x64 Development 构建通过（仅一处 `LNK1168` 被 qTinecmaTool 进程占用 .dll，强杀进程后重编 OK）。
- 已删除未使用的 `MM_RemoteLoadLibrary`（`/W4 -WX` 下 `C4505` 即报错）。
- target = `Wuthering Waves.exe`（ACE-Base 保护）；host = `qTinecmaTool.exe`，必须管理员。

## 4. 走过的 bug & fix（按出现顺序）

1. **`Couldn't find module 'TinecmaTool.dll'`**
   - 原因：thread-hijack `LoadLibraryW` 仍被 ACE-Base 拦，DLL 没真正进 PEB.Ldr。
   - 修：改走 manual map（不需要 `LoadLibrary`）。

2. **`LNK1168: 无法打开 ...\TinecmaTool.dll 进行写入`**
   - 原因：上一次 `qTinecmaTool.exe` / `TinecmaToolcmd.exe` 没退出，挂着 DLL handle。
   - 修：`Stop-Process -Id <pid> -Force` 后重编。常态化：每次重测前杀 helper 进程。

3. **`ThreadHijackCall(MM-LoadLibraryA): timed out waiting for done-flag`** + `ManualMap: couldn't resolve 'SleepConditionVariableSRW' in 'KERNEL32.dll'`
   - 原因：用 `CREATE_SUSPENDED` 创建的目标 loader 还没完全初始化，给 forwarder 派去执行 `LoadLibraryA(api-ms-win-...)` 直接卡死。
   - 修：新增 `MM_ResolveViaHost`。**host 端**用 `LoadLibraryExA(DONT_RESOLVE_DLL_REFERENCES) + GetProcAddress` 算 RVA，再 + target 模块 base，完全不让 target 执行代码。`MM_ResolveImports` / `MM_ResolveForwarder` 都改成优先走这条。

4. **`BuildCallShellcode` 栈错位（call 之前 RSP 必须 16B 对齐）**
   - 原因：之前 `sub rsp, 0x28`，加上 push 后破坏对齐。
   - 修：改 `sub rsp, 0x20`（4 个 register-home 槽，恰好 16B 对齐），call 前的栈正好满足 ABI。

5. **`warning C4505 "MM_RemoteLoadLibrary" 未引用` → `-WX` 升级为 error**
   - 原因：迁到 host-side resolve 后 `MM_RemoteLoadLibrary` 没人调。
   - 修：删除该函数。

6. **DLL 成功加载但 overlay 不出现，host 日志里没有 `ManualMap:` / `ThreadHijack:` 任何记录**
   - 原因：注册表残留 `AppInit_DLLs` + `LoadAppInit_DLLs=1` 让所有 user32 进程开机就吃 `TinecmaToolshim64.dll`，绕过 host 的 manual map 路径，配置也没注入。
   - 修：管理员 PowerShell：
     ```powershell
     Set-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows" -Name LoadAppInit_DLLs -Value 0
     Set-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Windows" -Name LoadAppInit_DLLs -Value 0
     ```
   - 备份脚本：仓库根 `TinecmaTool_RestoreGlobalHook.reg`（RenderDoc 自带）。

## 5. 已知遗留 / 后续 TODO

- **TLS only 注册到被劫持的那个线程**：target 后续新建线程访问 `__declspec(thread)` 变量会拿到 NULL；目前 RenderDoc capture hook 多在主渲染线程，未触发。若以后崩，需要 hook target 的 `BaseThreadInitThunk` 给新线程补 `TlsSetValue`。
- **delay-load imports 不预解析**：`__delayLoadHelper2` 命中时仍会 `LoadLibrary`；目前 TinecmaTool.dll 只 delay 系统 DLL（dxgi/d3d12），暂安全。
- **manual map 只走 x64**：32-bit target 自动跳过、回退到 thread-hijack。
- **manual-mapped 模块不在 PEB.Ldr / EnumProcessModules**：`FindRemoteDLL` 看不到，必须用 `InjectDLL` 的 `uintptr_t` 返回值。
- **host 必须自己加载过 TinecmaTool.dll**（qTinecmaTool.exe 自带依赖），不要从其他进程发起注入。
- **本会话尚未亲眼看到鸣潮 overlay**：上一次清完 AppInit_DLLs 后还没回测，下次开局先按 §6 自检流程跑一遍。

## 6. 下次复现 / 自检最小步骤

1. 确认 `LoadAppInit_DLLs` 在两条注册表路径均为 0。
2. 启动 host：以管理员运行 `x64\Development\qTinecmaTool.exe`。
3. 启动 target：从 host 内"启动可执行文件"加载 `Wuthering Waves.exe`。
4. 看 host 日志 `%TEMP%\TinecmaTool\qTinecmaTool_*.log`，期望关键字：
   - `ManualMap: ... mapped at 0x... (size=..., delta=...)`
   - `ManualMap: TLS slot N allocated for module @ 0x...`
   - **不应**再出现 `Couldn't find module 'TinecmaTool.dll'`。
5. 看 target 日志 `%TEMP%\TinecmaTool\Wuthering Waves_*.log`，期望出现 capture client / overlay 注册等行。
6. 游戏内左上角应能看到 overlay，按配置的 capture key 截帧。

如果第 4 步没有 `ManualMap:` 行，先排查：是否被 AppInit_DLLs 再次启用；是否 host 没拿到管理员；是否目标进程在 32-bit。
