# 线程劫持注入（SetThreadContext）—— 绕过 CrashSight

> 分支：`tinecmatool/threadctx-inject`（基于 `endfield/import`）
> 基础：[`BRANDING_TINECMATOOL.md`](BRANDING_TINECMATOOL.md)（第一步：改特征码）
> 参考：知乎文章方案的第二步

## 1. 为什么要改

`endfield/import` 分支已经把所有品牌特征统一为 `TinecmaTool`，绕过了反作弊对 **静态字符串**（`renderdoc`、`qrenderdoc.exe`、`RenderDoc_*` 命名内核对象 …）的扫描。

但当 `qTinecmaTool.exe` 启动目标进程并注入 `TinecmaTool.dll` 时，原 RenderDoc 用的仍然是经典的：

```text
VirtualAllocEx(remote)              ← 远端分配
WriteProcessMemory(remote, dllPath) ← 写 DLL 路径
CreateRemoteThread(LoadLibraryW)    ← ❌ 反作弊 hook 点
```

**`CreateRemoteThread` 是 CrashSight / EAC / BattlEye 等用户态反作弊最经典的 IOC（Indicator of Compromise）**。它们通常 hook 这些位置之一：

- `kernel32!CreateRemoteThread` / `CreateRemoteThreadEx`
- `ntdll!NtCreateThreadEx`
- DLL 加载回调（`LdrRegisterDllNotification`），任何新加载的 DLL 都会触发，回调里检查"是否是非白名单 DLL 通过远程线程加载"

只要看到一个新远程线程的入口指向 `LoadLibraryW`/`LoadLibraryA` 且参数指向自分配的内存，立刻判定为外部注入。

## 2. 改成什么

本分支引入 **线程劫持（SetThreadContext / thread-context hijack）** 注入：

```text
1. 枚举目标 PID 的现有线程，挑一个
2. SuspendThread + GetThreadContext            ← 抓 CPU 快照
3. VirtualAllocEx 一段可执行 shellcode          ← 含 LoadLibrary 调用 + 跳回
4. SetThreadContext 把 RIP/EIP 改成 shellcode  ← ❌ 不创建任何新线程
5. ResumeThread                                ← 受害线程自己执行注入
6. 轮询 FindRemoteDLL / done-flag 等完成
7. VirtualFreeEx 清理
```

关键差异：

| | CreateRemoteThread | SetThreadContext 劫持 |
|---|---|---|
| 调用 `CreateRemoteThread*` | ✅（被 hook） | ❌ |
| 创建新线程 | ✅ | ❌ |
| `LoadLibraryW` 的调用栈 | 全新工作线程（栈空） | 受害线程的真实调用栈 |
| 反作弊判定难度 | 极易 | 需对比 RIP 跳转、检测 SetThreadContext 异常路径 |
| 需要权限 | `PROCESS_CREATE_THREAD` | `THREAD_GET_CONTEXT / SET_CONTEXT / SUSPEND_RESUME` |

## 3. 实现位置

全部改动集中在 **一个文件**：

- [`renderdoc/os/win32/win32_process.cpp`](renderdoc/os/win32/win32_process.cpp)

### 3.1 新增（在原 `InjectDLL` 函数前，约 252~706 行）

| 标识符 | 作用 |
|---|---|
| `TINECMATOOL_USE_THREADHIJACK_INJECT` 宏 | 默认 `1`；定义为 `0` 可回退到原 `CreateRemoteThread` 实现 |
| `BuildHijackShellcode(arg, funcAddr, doneFlagAddr, origRip)` | 拼出 x64 / x86 trampoline 字节序列。`doneFlagAddr=0` 时不写完成标志 |
| `OpenInjectionThread(pid)` | 用 `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 找一个可挂起线程 |
| `ThreadHijackInvoke(hProcess, pid, funcAddr, argRemote, dataOut, dataLen, debugTag)` | 通用执行器：挂起 → 写 shellcode → 改 RIP → 恢复 → 等完成 |
| `InjectDLL_ThreadHijack(hProcess, pid, libName)` | 包装 `LoadLibraryW(dllPath)` |
| `InjectFunctionCall_ThreadHijack(hProcess, pid, funcAddr, data, len, tag)` | 包装任意 `INTERNAL_*` 导出（带 done-flag + 读回 data） |

### 3.2 现有 `InjectDLL` / `InjectFunctionCall` 改造

**签名完全保留**，函数体首部加 `#if TINECMATOOL_USE_THREADHIJACK_INJECT` 分支，优先调用 hijack 版本；失败再降级到原 `CreateRemoteThread` 路径。

这意味着 `InjectIntoProcess` 里的 **10+ 个调用点完全不动**。

## 4. Shellcode 详解

### x64 版本（约 68 字节，无 done-flag；80 字节带 done-flag）

```asm
pushfq                        ; 9C
push rax/rcx/rdx              ; 50 51 52
push r8/r9/r10/r11            ; 41 50 / 41 51 / 41 52 / 41 53
sub rsp, 0x28                 ; 48 83 EC 28        ← 32B 阴影空间 + 16B 栈对齐
mov rcx, <arg>                ; 48 B9 [8 bytes]    ← 第 1 个参数（Win64 ABI）
mov rax, <funcAddr>           ; 48 B8 [8 bytes]
call rax                      ; FF D0
add rsp, 0x28                 ; 48 83 C4 28

;; （仅 function-call 变体）
mov rax, <doneFlagAddr>       ; 48 B8 [8 bytes]
mov byte [rax], 1             ; C6 00 01

pop r11/r10/r9/r8/rdx/rcx/rax ; 41 5B ... 58
popfq                         ; 9D

;; 跳回原 RIP（用 push imm32 + mov hi32 + ret，避免破坏寄存器）
push <origRip 低 32>          ; 68 [4 bytes]
mov [rsp+4], <origRip 高 32>  ; C7 44 24 04 [4 bytes]
ret                           ; C3
```

### x86 版本（约 22 字节，无 done-flag；29 字节带 done-flag）

```asm
pushfd                        ; 9C
pushad                        ; 60
push <arg>                    ; 68 [4 bytes]   ← stdcall：callee 清栈
mov eax, <funcAddr>           ; B8 [4 bytes]
call eax                      ; FF D0

;; （仅 function-call 变体）
mov byte [<doneFlagAddr>], 1  ; C6 05 [4 bytes] 01

popad                         ; 61
popfd                         ; 9D
push <origEip>                ; 68 [4 bytes]
ret                           ; C3
```

### 设计要点

1. **完整保存所有 volatile 寄存器和 flags**。Win64 调用约定中 volatile：`RAX, RCX, RDX, R8-R11`；非 volatile（`RBX, RBP, RDI, RSI, R12-R15, XMM6-15`）由被调函数保护。x86 用 `pushad/popad` 一把推完（含 `EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP`，pop 时跳过 ESP）。
2. **`sub rsp, 0x28` 而非 0x20**：阴影空间 32 字节 + 1 个对齐 8 字节，是 Win64 ABI 强制要求；同时保证 `call` 后 `RSP & 0xF == 0`。
3. **跳回原 RIP 不用 `mov rax, X; jmp rax`**：那样会破坏 `rax`。改用 `push imm32 + mov [rsp+4], imm32 + ret`，`ret` 弹栈作为返回地址，rax 不动。
4. **Done-flag 是单字节**：x86/x64 上字节对齐写入天然原子，不需要 `lock` 前缀；主控端 `ReadProcessMemory(1 byte)` 轮询。
5. **`LoadLibraryW` 地址跨进程有效**：kernel32 在同一会话所有进程的基址相同（除非启用 IndependentASLR），本地 `GetProcAddress` 拿到的指针在远端可直接用。这是原 RenderDoc 已有的假设。
6. **位宽匹配**：32-bit 目标 / 64-bit 注入器场景由 RenderDoc 原有的"farm out to alternate `TinecmaToolcmd.exe`"机制处理，shellcode 与注入器位宽永远一致。

## 5. 选择劫持线程

```cpp
HANDLE OpenInjectionThread(DWORD pid)
{
  // 枚举所有线程，挑第一个属于 pid 的，OpenThread 拿到 handle
  // 当前实现：直接选第一个（通常是主线程）
}
```

后续可优化：

- **CREATE_SUSPENDED 场景**：`LaunchAndInjectIntoProcess` 走的就是这条路。主线程 RIP 处于 `ntdll!LdrInitializeThunk`，**直接劫持会破坏 Loader 初始化** → `LoadLibraryW` 内部抢 Loader Lock 时死锁。当前缓解：默认场景下原 RenderDoc 已经把 hook 注入设计为 "先放过 Loader 再注入"，所以一般 OK；若死锁可：
  - 先 `ResumeThread` 让目标跑 100~500ms 后再 `SuspendThread + 劫持`，或
  - 等目标进程出现第二个线程（ntdll worker），劫持那个非 Loader 线程。
- **多线程进程**：可加权选 GUI 线程（最常活跃）或 alertable wait 中的线程。

当前实现的故障模式：
- 劫持失败 → `ThreadHijackInvoke` 返回 `false` → 上层 `InjectDLL` 回落到 `CreateRemoteThread` → 反作弊看到，但保证功能不丢。

## 6. 编译开关

| 取值 | 效果 |
|---|---|
| `TINECMATOOL_USE_THREADHIJACK_INJECT = 1`（默认） | hijack 优先；失败 fallback 到 `CreateRemoteThread` |
| `TINECMATOOL_USE_THREADHIJACK_INJECT = 0` | 完全走原 `CreateRemoteThread` 路径（等同 RenderDoc 上游行为） |

在 `vcxproj` 的 `<PreprocessorDefinitions>` 加 `TINECMATOOL_USE_THREADHIJACK_INJECT=0` 即可关闭。

## 7. 测试 / 验证

1. 重编 `renderdoc.sln`（x64 / Development），产物 `x64\Development\TinecmaTool.dll`、`TinecmaToolcmd.exe`、`qTinecmaTool.exe`。
2. 启动 `qTinecmaTool.exe` → File → Launch Application → 选 `Wuwa\Client\Binaries\Win64\Client-Win64-Shipping.exe`。
3. 若注入成功，UI 左下角出现 connection bar + 帧计数；F12 抓帧。
4. **抓不到 / 进程立刻退出**：
   - 看 `%APPDATA%\TinecmaTool\rdoc_*.log`，搜 `ThreadHijack` 关键字
   - 若看到 `falling back to CreateRemoteThread`，说明劫持失败，可能：
     - 目标线程权限不够（系统进程 → 需以管理员运行 `qTinecmaTool.exe`）
     - SetThreadContext 被反作弊 hook 拦截
   - 若看到 `timed out waiting for done-flag`，说明 shellcode 没成功执行 → 看下面 Debug 建议

## 8. Debug 建议（shellcode 不工作时）

- **把 `kHijackTimeoutMs` 临时调高到 60000，重编看是否只是慢**
- **临时设置 `TINECMATOOL_USE_THREADHIJACK_INJECT=0` 退回 `CreateRemoteThread`**，验证基础流程仍 OK
- **附加 WinDbg 到目标进程**：
  - `bp ntdll!LdrLoadDll` 看 `LoadLibraryW` 是否被调到
  - `kP` 看调用栈是不是从我们的 shellcode 跳过去的
- **检查 done-flag 内存**：在 WinDbg 用 `db <doneFlagAddr> L1`，劫持后应该变 `01`
- **比对 RIP**：劫持瞬间 `~* k` 看哪个线程 RIP 在 shellcode 区域

## 9. 后续可加的对抗（如本方案被反作弊跟进识别）

- 用 **APC 注入**（`QueueUserAPC` + `NtAlertResumeThread`）替代 SetThreadContext
- 反向：用 **`MapViewOfFile` + 远端线程改 ImageBase** 技术（图像映射注入）
- 用 **`NtSetContextThread` 直接走 syscall**，绕过 user-mode hook
- 把 LoadLibraryW 调用换成 **手工映射 DLL（reflective loader）**，连 LoadLibrary 都不调
- 在 shellcode 里 **用 `RtlInsertElementGenericTable` 等冷门 ntdll API 间接触发 DLL 加载**（hook 列表盲区）

这些都是后续分支的事，本分支只完成 mos9527 / 知乎文章描述的"SetThreadContext 替代 CreateRemoteThread"基础方案。

## 10. 文件清单

| 文件 | 变更 |
|---|---|
| `renderdoc/os/win32/win32_process.cpp` | +455 / -2 行（新增 shellcode 拼装 + 线程劫持 + 分发宏；保留旧实现为 fallback） |
| `TINECMATOOL_THREADCTX_INJECT.md` | 新增（本文件） |
| `.cursor/rules/project-overview.mdc` | 标注新分支与开关 |

任何其它源码、vcxproj、品牌字符串 **均未触及**。
