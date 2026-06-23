# TinecmaTool — manual-map DLL 注入

## 背景

`tinecmatool/threadctx-inject` 分支把 RenderDoc 的注入路径从 `CreateRemoteThread(LoadLibraryW)` 换成 **线程劫持 + SetThreadContext**，已经能绕开 CrashSight 这种 user-mode 反作弊对 `CreateRemoteThread` 的钩子。但对鸣潮（Wuthering Waves）那类 **kernel-mode 反作弊**（ACE-Base 通过 `PsSetLoadImageNotifyRoutine` / `ObRegisterCallbacks` 阻断未签名 DLL 进入受保护进程），`LoadLibraryW` 本身就拿不到我们的镜像，所以线程劫持也救不了。

`tinecmatool/manualmap-inject` 分支在 thread-hijack 之上再加一层 **手动 PE 镜像加载（manual map）**：完全不调用 `LoadLibrary`，host 直接在目标进程里 alloc + 写镜像 + 修 reloc/IAT，最后用线程劫持跳到 DllMain。

## 分支关系

```
endfield/import
  └─ tinecmatool/threadctx-inject     # 线程劫持
       └─ tinecmatool/manualmap-inject # 手动映射  ← 本分支
```

主仓库（`https://github.com/Tinenc/GraphicsDebuggerRdcTools.git`）的 `tinecmatool/threadctx-inject` 是基线，此分支 cherry-pick 自该基线再加 manual map 实现。

## 编译宏

| 宏 | 默认 | 说明 |
|---|---|---|
| `TINECMATOOL_USE_MANUALMAP_INJECT` | `1` | 启用手动映射作为首选路径 |
| `TINECMATOOL_USE_THREADHIJACK_INJECT` | `1` | manual map 失败时回退到线程劫持 |
| 都关 | — | 走最老的 `CreateRemoteThread(LoadLibraryW)`（会被 CrashSight / ACE 拦） |

回退链（出错自动降级）：

```
manual map  →  thread-hijack LoadLibraryW  →  CreateRemoteThread LoadLibraryW
```

每级失败会在 `%TEMP%\TinecmaTool\<host-exe>_*.log` 里写 `Warning` 日志，方便定位走到了哪条路径。

## 工作流程

注入流程（x64，host = `qTinecmaTool.exe`，target = 游戏进程）：

1. **读 PE**：host 把磁盘上的 `TinecmaTool.dll` 一次性读入内存，校验 `IMAGE_DOS_SIGNATURE` / `IMAGE_NT_SIGNATURE` / `IMAGE_FILE_MACHINE_AMD64`。
2. **构建 shadow image**：在 host 端 alloc `SizeOfImage` 字节，按 section header 把 raw data 按 VA 展开。
3. **远端分配**：`VirtualAllocEx(target, preferredImageBase, SizeOfImage, PAGE_READWRITE)`，失败则让系统自选基址（接受 ASLR）。
4. **应用 base relocations**：根据实际分配地址与 `OptionalHeader.ImageBase` 的差，遍历 `.reloc` 把 shadow 中的 `IMAGE_REL_BASED_DIR64` / `HIGHLOW` 修正。
5. **解析导入表**：对每个依赖 DLL：
   - **优先 host-side 解析**（`MM_ResolveViaHost`）：在 host 进程里 `LoadLibraryExA(dll, DONT_RESOLVE_DLL_REFERENCES)` + `GetProcAddress` 拿到导出函数指针，再换算成 `RVA`，最后在 target 进程里按 basename 找同名模块基址（`Toolhelp32Snapshot`），算出 `targetBase + RVA`。这条路径覆盖系统 DLL 和 **API Set DLLs**（`api-ms-win-*`），完全不需要在 target 里执行任何代码 —— 避免 `CREATE_SUSPENDED` 状态下目标进程 loader 未初始化时调 `LoadLibraryA` 死锁。
   - **回退 target-side 解析**：仅当 target 已经加载了某个非系统模块（host 端没有同名 DLL），才会用 `ReadProcessMemory` 直接读 target 中那个 DLL 的 `IMAGE_EXPORT_DIRECTORY` 查名字 / 序号。
   - 处理 forwarder（`"NTDLL.RtlXxx"` 这种），同样优先走 `MM_ResolveViaHost`，递归深度 6 上限。
   - 把每个 IAT thunk 改成 `targetModuleBase + exportRVA`。
6. **WriteProcessMemory**：一次性把 shadow 整块写到 target。
7. **保护属性**：按每个 section 的 `Characteristics` 调 `VirtualProtectEx`（X|R|W → `PAGE_EXECUTE_READWRITE` 等）。
8. **TLS slot**：调 target 的 `TlsAlloc()` 拿一个 slot index，patch 模块内的 `_tls_index`，分配 raw-data 块、写 `TlsSetValue(slot, ptr)`（仅给被劫持的那个线程注册，见下方"已知限制"）。
9. **异常表注册**：调 `RtlAddFunctionTable(remote .pdata, count, base)`，让 C++ SEH unwind / try-catch 能在 manual-mapped 模块内工作。
10. **TLS callbacks**：遍历 `IMAGE_TLS_DIRECTORY.AddressOfCallBacks`，对每个 cb 调 `cb(base, DLL_PROCESS_ATTACH, NULL)`。
11. **DllMain**：调 `EntryPoint(base, DLL_PROCESS_ATTACH, NULL)`。返回 `FALSE` 视为失败。

整个流程**不调用 `LoadLibrary`**，所以 `PsSetLoadImageNotifyRoutine` 不会触发，模块也**不会**进入 `PEB.Ldr`、`EnumProcessModules`、`GetModuleHandle` —— 这正是绕开 ACE-Base 模块扫描的关键。

## 上层适配

因为 manual-mapped 模块不在 `PEB.Ldr` 中，`FindRemoteDLL` 用 `Toolhelp32Snapshot` 永远找不到它。`InjectDLL` 的签名因此改为：

```cpp
uintptr_t InjectDLL(HANDLE hProcess, rdcwstr libName);
// 返回值: 成功 manual-map 时返回 remote base，
//        走 thread-hijack 或 CreateRemoteThread 成功时返回 0（caller 用 FindRemoteDLL）。
```

调用方（`win32_process.cpp::StartGlobalHook` 内部）：

```cpp
uintptr_t manualMapBase = InjectDLL(hProcess, renderdocPath);
uintptr_t loc = manualMapBase ? manualMapBase
                              : FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");
```

后续的 `InjectFunctionCall(hProcess, loc, "INTERNAL_*", ...)` 不用改 —— 它本来就用
`func_local + remote_base - host_base` 算出 export 在 target 中的地址，且 host 端有同一份
`TinecmaTool.dll` 已加载（qTinecmaTool.exe 依赖 TinecmaTool.dll 的 SWIG export），RVA 完全一致。

## 已知限制

1. **TLS 只对劫持线程注册**：`TlsSetValue` 是 per-thread 的，我们只给注入时被劫持的那个线程
   写了 TLS slot。target 后续自己创建的新线程访问 `thread_local` / `__declspec(thread)` 变量
   时会拿到 `NULL`。RenderDoc capture 钩子主要在主渲染线程跑，实际未触发；如果遇到崩溃，需要
   补一个 hook target 的 `BaseThreadInitThunk` / `CreateThread` 来给新线程注册 TLS。
2. **Delay-load imports 不预解析**：`__delayLoadHelper2` 第一次调用对应函数时仍然走 `LoadLibrary`，
   如果延迟加载的目标 DLL 被 ACE 拦截，那条延迟链上的函数会爆。目前 TinecmaTool.dll 的 delay
   imports 均为系统 DLL（dxgi/d3d12 等），系统签名 DLL 不在 ACE 黑名单内，所以暂时不是问题。
3. **只支持 x64**：32-bit target 自动跳过 manual map（`InjectDLL_ManualMap` 直接返回 0），
   回退到 thread-hijack。
4. **HostProcess 必须先加载过 TinecmaTool.dll**：`InjectFunctionCall` 的 `GetModuleHandleA`
   需要在 host 进程里能找到这个 DLL；qTinecmaTool.exe 通过 stub 依赖它，自然成立；不要尝试从
   一个未加载 TinecmaTool.dll 的进程发起注入。
5. **DLL 必须可读**：host 进程要有权限读 `TinecmaTool.dll` 文件（路径来自 `GetModuleFileNameW`），
   如果是把 DLL 放在 `Program Files` 下又以非管理员运行 host，会因为权限读不到。当前 host 已是
   管理员（`Running as administrator`），不影响。

## 关闭 AppInit_DLLs 全局钩子（重要）

老版 RenderDoc 的 "全局钩子" 功能会把 `TinecmaToolshim64/32.dll` 的短路径写入注册表

```
HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows\AppInit_DLLs
HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Windows\AppInit_DLLs
```

并把 `LoadAppInit_DLLs` 置 1，让所有 `user32` 进程开机就加载该 shim → 自动 LoadLibrary `TinecmaTool.dll`。

这条全局加载路径会**绕过我们的 manual map**：

- 进程一启动就吃到 DLL，主线程还没运行；
- DLL 走的是普通 `LoadLibrary`，会被 ACE-Base 的 `PsSetLoadImageNotifyRoutine` / `ObRegisterCallbacks` 命中，从而被 kill 或者拒绝加载；
- 即便侥幸加载，也没有 `INTERNAL_SetCaptureOptions` 等配置注入步骤，overlay 不会出现，capture key 也读不到 host 那份。

诊断现象：host 日志里没有 `ManualMap:` / `ThreadHijack:` 任何字样，但 target 自己 `TinecmaTool/<exe>_*.log` 里出现 "successfully loaded" + capture client 监听端口启动，这就是被 AppInit 全局钩子吃到的标志。

**处理方法**（管理员 PowerShell）：

```powershell
Set-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows" -Name LoadAppInit_DLLs -Value 0
Set-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Windows" -Name LoadAppInit_DLLs -Value 0
# 可选：备份后清空两路 AppInit_DLLs 字符串
```

清完后重启目标进程即可让 host 的 manual-map 路径真正生效。注册表备份文件：仓库根目录的 `TinecmaTool_RestoreGlobalHook.reg`（由 RenderDoc 自身在 `StartGlobalHook` 时生成）。

## 调试

- 走 manual map 成功时，host 日志里会有：
  ```
  ManualMap: 'C:\...\TinecmaTool.dll' mapped at 0x<base> (size=<bytes>, delta=<reloc-delta>)
  ManualMap: TLS slot <N> allocated for module @ 0x<base>
  ```
  且原本的 `Couldn't find module 'TinecmaTool.dll' among N modules` 错误**消失**。
- 走失败回退时会看到：
  ```
  Manual-map inject failed for '...'; falling back to thread-hijack LoadLibrary
  ```
- 如果连 thread-hijack 都被 ACE 拦，那才会进入旧的 `CreateRemoteThread` + 现成的"找不到模块"报错。
- target 进程里 TinecmaTool 自报告日志在 `%TEMP%\TinecmaTool\<target-exe>_*.log`，注意因为模块不在
  PEB.Ldr 中，`GetModuleFileNameW(GetModuleHandle("TinecmaTool.dll"), ...)` 在 target 进程中**会拿不到
  路径**，导致 `win32_stringio.cpp` 里的某些路径解析依赖会回退到默认值；如果出现这类问题，单独修
  那一行即可（host 注入时通过 `INTERNAL_SetDebugLogFile` 显式指定 log 路径，足以走通）。

## 实现位置

全部在 `renderdoc/os/win32/win32_process.cpp` 同一个 anonymous namespace 块里：

- `InjectDLL_ManualMap` — 主入口
- `MM_ReadFileBytes` / `MM_ReadRemote` / `MM_ReadRemoteCString` — IO 工具
- `MM_FindRemoteModuleBase` — Toolhelp32 模块查找
- `BuildCallShellcode` / `ThreadHijackCall` — 支持多参数 + 返回值捕获的劫持调用（栈对齐为 16B，`sub rsp, 0x20` 留 shadow space）
- `MM_ResolveViaHost` — host 端 `LoadLibraryExA(DONT_RESOLVE_DLL_REFERENCES)` + `GetProcAddress`，换算 RVA 后映射到 target 模块基址；用于系统 DLL / API Set 导入解析（**首选路径**）
- `MM_RemoteResolveExport` / `MM_ResolveForwarder` — 远端 export 查找 + forwarder 递归（host 端解析失败时回退）
- `MM_ApplyRelocations` — `.reloc` 处理
- `MM_ResolveImports` — IAT patching
- `MM_SectionToProtection` / `MM_ApplyProtections` — section 权限
- `MM_SetupTls` / `MM_GatherTlsCallbacks` — TLS slot / callbacks
- `MM_RegisterExceptionTable` — `RtlAddFunctionTable`

