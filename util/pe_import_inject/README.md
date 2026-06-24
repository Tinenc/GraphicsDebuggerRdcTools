# `pe_import_inject` — 静态 PE Import Table 注入

把一项 `IMAGE_IMPORT_DESCRIPTOR` 写进目标 EXE/DLL，让 Windows 自己的 PE
loader (`ntdll!LdrInitializeThunk`) 把我们的 hook DLL 当成"正常静态依赖"
加载进目标进程。

## 这套思路解决什么

参考 [mos9527 的博客](https://mos9527.github.io/posts/renderdoc-asi-loader/)
"RenderDoc 抓帧 Steam 及带启动器游戏通解"，思路是不再让 host 端做
`OpenProcess + SuspendThread + SetThreadContext + VirtualAllocEx` 那一套
显眼的操作，而是**在 game 启动之前先改 PE 文件**，把我们的 DLL 加进 Import
Directory，让 game 自己的 ntdll loader 帮我们 LoadLibrary。

相比 manual map / thread-hijack 注入，优势：

- 不触发 user-mode anti-cheat 对 `CreateRemoteThread` / `NtSetContextThread`
  / `VirtualAllocEx` 的 hook。
- DLL 正常进 `PEB.Ldr`，VAD 树正常，没有 "幽灵镜像"。
- 在 LdrInitializeThunk 拓扑序里加载，**早于** target 自身代码（包括它的
  static / delay-load 反作弊模块）。

劣势：

- 改动 PE 文件本身 — anti-tamper 哈希校验会抓。这一点没法绕，是 by-design。
- 不适用于走 launcher 拉游戏的场景（要 patch 的是 launcher 还是 game？需要
  从依赖图看清楚）。

## 文件清单

| 文件 | 说明 |
|---|---|
| `inject.ps1`                       | PowerShell 5.1+ 脚本，纯标准库，无外部依赖。在目标 PE32+ 镜像里追加一段 `.tnc` section 并改写 Import Directory。 |
| `poc_dll/TinecmaTool_PoC.cpp`      | 最小 DllMain，DLL_PROCESS_ATTACH 时往 `%TEMP%\TinecmaTool_PoC\<exe>_<pid>.log` 写 PEB.Ldr 当前已加载模块列表 — 验证 ntdll loader 真的在调我们而且能看到具体加载顺序。 |
| `poc_dll/build.bat`                | 用 VS 2022 Community 的 cl.exe 编 PoC DLL（找不到 VS 时会自报错）。 |
| `poc_dll/hello.cpp` + `build_hello.bat` | sanity-test 用的 hello world EXE，让我们能不动 system32 自测 patcher。 |

## 快速上手 — sanity test

```powershell
# 1. 编 PoC DLL
cd util\pe_import_inject\poc_dll
.\build.bat                    # 输出 build\TinecmaTool_PoC.dll

# 2. 编 hello.exe（独立小 EXE，方便测试 patcher）
.\build_hello.bat              # 输出 build\hello.exe

# 3. 准备一个 sandbox 目录（不要在 game 目录直接折腾，先就地验证 patcher）
$sb = "$env:TEMP\tinecma_pe_test"
ni $sb -ItemType Directory -Force | Out-Null
cp .\build\hello.exe              $sb\
cp .\build\TinecmaTool_PoC.dll    $sb\

# 4. patch hello.exe 加 import TinecmaTool_PoC.dll
cd ..
powershell -ExecutionPolicy Bypass -File .\inject.ps1 `
    -InputFile  "$sb\hello.exe" `
    -OutputFile "$sb\hello_patched.exe" `
    -InjectDll  "TinecmaTool_PoC.dll" `
    -ImportSymbol "TnT_Entry"

# 5. 跑 patched exe
& "$sb\hello_patched.exe"
# stdout: hello.exe pid=12345 argc=1   (说明主流程 OK)

# 6. 看 PoC log
gci "$env:TEMP\TinecmaTool_PoC"
gc  "$env:TEMP\TinecmaTool_PoC\hello_patched.exe_*.log"
```

期望 log 末尾看到类似：

```
[YYYY-MM-DD hh:mm:ss.fff] pid=XXX === DLL_PROCESS_ATTACH ===
[...] exe: ...\hello_patched.exe
[...] self: ...\TinecmaTool_PoC.dll @ 0x...
[...] PEB.Ldr modules (in-load-order):
[...]   [ 0] ... hello_patched.exe
[...]   [ 1] ... ntdll.dll
[...]   [ 2] ... KERNEL32.DLL
[...]   [ 3] ... KERNELBASE.dll
[...]   [ 4] ... ucrtbase.dll
[...]   [ 5] ... VCRUNTIME140.dll
[...]   [ 6] ... TinecmaTool_PoC.dll          ← 我们
```

如果 patched exe 退出码是 `-1073741819` (`0xC0000005`)：通常是 section
characteristics 没带写位（loader 写 IAT 会 AV）。`inject.ps1` 已经默认带
`IMAGE_SCN_MEM_WRITE`，这里只是提示一下排查路径。

## 应用到目标游戏（鸣潮 Wuthering Waves 为例）

依赖关系（dumpbin /dependents）：

```
launcher.exe          ──► KERNEL32, SHELL32                  (无入口面)
Wuthering Waves.exe   ──► KERNEL32, USER32, ADVAPI32, SHELL32, SHLWAPI  (无入口面)

Client-Win64-Shipping.exe
  ──► Client-Win64-ShippingBase.dll      ★ 唯一非 system 静态依赖
  ──► 30+ delay-load (PhysX, D3D12, CEF, XAudio2, ...)
```

可选 patch 目标：

| 文件 | 优点 | 缺点 |
|---|---|---|
| `Client-Win64-Shipping.exe`        | 最早入口；TinecmaTool 在 ShippingBase 之前加载 | 反篡改首要校验目标 |
| `Client-Win64-ShippingBase.dll`    | 改 DLL 没改 EXE，部分反作弊只校 EXE | TinecmaTool 在 ShippingBase 自身的 DllMain *之前*（loader 拓扑序），但晚于 ShippingBase 进入 PEB |

```powershell
# 推荐：先 patch DLL，不动 EXE
$game = 'D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64'

# 备份
cp $game\Client-Win64-ShippingBase.dll $game\Client-Win64-ShippingBase.dll.orig

# 把 TinecmaTool.dll 也放进 game 目录（loader 搜 app dir 优先）
cp '...\x64\Development\TinecmaTool.dll' "$game\TinecmaTool.dll"

# patch DLL（输出到临时位置，确认没问题再覆盖）
powershell -ExecutionPolicy Bypass -File .\inject.ps1 `
    -InputFile  "$game\Client-Win64-ShippingBase.dll.orig" `
    -OutputFile "$game\Client-Win64-ShippingBase.dll" `
    -InjectDll  'TinecmaTool.dll' `
    -ImportSymbol 'TINECMATOOL_GetAPI'
```

`ImportSymbol` 选 TinecmaTool.dll 的某个真实导出（`TINECMATOOL_GetAPI` 在
品牌统一时确保导出，可以用 `dumpbin /exports x64\Development\TinecmaTool.dll`
确认；也可以保留 PoC 的 `TnT_Entry` 然后用 PoC 先验证一步）。

## 已知限制 / 风险

1. **anti-tamper hash 校验**：ACE-Base / 类似的反作弊会算 image hash 跟服务
   端比对；改 PE 必然挂。这一步走通了再想下一步（可能要 hook 校验 API，但
   那就回到注入大改的路上）。
2. **数字签名**：被 patch 的 PE 失去原签名。如果 anti-cheat 校验签名链，挂。
3. **delay-load**：本工具只动 static `IMAGE_DIRECTORY_ENTRY_IMPORT`，没动
   `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT`。
4. **bound-import**：脚本会清掉 `IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT`（旧的
   bind 缓存对 patch 过的 IDT 是错的），loader 会重做一次绑定。这点对 Win10+
   构建几乎都是 no-op。
5. **PE checksum**：用户态 EXE/DLL 不强制校验，脚本直接清零。kernel image
   / driver 不要用这个脚本。
6. **32-bit**：不支持（脚本里直接 throw）。
7. **section header 满**：极少数手工裁过 PE 头部的 image 没空间塞新 section
   header。脚本会 throw 提示。

## 这条线和 manualmap-inject / threadctx-inject 的关系

完全独立。`tinecmatool/threadctx-inject` 和 `tinecmatool/manualmap-inject`
分支改的是 host 端 `Process::InjectIntoProcess` 的逻辑；本分支
`tinecmatool/pe-import-inject` 是 **on-disk 工具**，跟 host 注入路径无关，
甚至不需要 qTinecmaTool.exe 启动 — 用户直接 patch 文件、双击运行 game.exe
即可。

把它当作"自动化的 ASI Loader 变种"理解就好。
