# 2026-06-25 · PE Import 注入 + Wuwa launcher md5 校验绕过

接续 [`2026-06-24-manualmap-inject-611501bb.md`](./2026-06-24-manualmap-inject-611501bb.md)，
同一 chat (`611501bb`) 在 manual map / thread-hijack 注入被 ACE-Base 用户态拦截后，
全面切换到「on-disk PE Import Table 注入」+「launcher 端 md5 校验绕过」的双层方案。

## 需求

1. manual-map 在 Wuwa 上挂在 `VirtualAllocEx(76 MB) → ERROR_ACCESS_DENIED (5)`，
   ACE-Base 在内核态剥了 `PROCESS_VM_OPERATION`，并 kill 掉 launcher。**用户态注入这条线已死**。
2. 用户提供了 [mos9527 的博客](https://mos9527.github.io/posts/renderdoc-asi-loader/)，
   选择「PE Import Table 注入（ASI Loader 风格）」：直接 patch game/launcher 的 PE
   文件，把 TinecmaTool DLL 加进 Import Directory，让 Windows 自己的 ntdll loader
   帮我们 LoadLibrary。
3. patch 跑通后遇到 launcher 弹"游戏文件缺失或损坏"，需要反编译 launcher 找校验
   逻辑并绕过。

## 决策

- **on-disk patch**：在 PE32+ 镜像里追加 `.tnc` section，section 头存
  `IMAGE_IMPORT_DESCRIPTOR` + `IMAGE_THUNK_DATA64` + DLL/Hint-Name 字符串，
  改写 `IMAGE_DIRECTORY_ENTRY_IMPORT` 指向新表，清掉 BOUND_IMPORT 缓存和
  PE checksum；用 PowerShell 5.1 标准库实现，零外部依赖。
- **目标选 `Client-Win64-ShippingBase.dll`**（不是 EXE）：从 `dumpbin /dependents`
  看它是 Shipping.exe 唯一非 system static dep，patch DLL 影响面小。
- **launcher 校验**：反编译 `launcher_main.dll`（1.07 MB .NET 8 PE32+ 托管 dll），
  263 个 .cs 文件输出。定位到 `KRResources.KRResCheckFlow.Exec()`，由 WebView H5
  前端通过 `kr_check_res_valid` IPC 触发。用 dnlib 改 IL 把方法体替换成单一
  `resultCallback?.Invoke(true, 0, "...", null)`，永远 success。
- **patcher 设计**：.NET 9 console + dnlib 4.4.0，独立工具；**绝不**把库洛的
  launcher_main.dll 或 263 个反编译 .cs 文件 commit 进仓库——版权风险，仓库
  公开。用户自带合法授权的鸣潮安装，本地反编译/patch。

## 改动文件（本次提交）

| 路径 | 用途 |
|---|---|
| `util/pe_import_inject/README.md` | 顶层说明追加「launcher md5 校验绕过」章节 + launcher_patcher 文件清单。 |
| `util/pe_import_inject/launcher_patcher/Program.cs` | dnlib-based IL patcher，替换 `KRResCheckFlow.Exec` 方法体。 |
| `util/pe_import_inject/launcher_patcher/LauncherPatcher.csproj` | .NET 9 console + dnlib 4.4.0。 |
| `util/pe_import_inject/launcher_patcher/apply_launcher_patch.ps1` | 备份/安装/还原 wrapper，交互式确认。 |
| `util/pe_import_inject/launcher_patcher/README.md` | 子目录说明 + 用法。 |
| `util/pe_import_inject/launcher_patcher/.gitignore` | 排除 bin/ obj/ 和所有 launcher_*.dll 衍生品。 |
| `.gitattributes` | 加 `*.jsonl -text`（agent transcript 标 binary 不做 CRLF 规范化）。 |
| `agent-chats/README.md` | 加本次归档行。 |
| `agent-chats/2026-06-25-pe-import-inject-launcher-611501bb.{jsonl,md}` | 原始 transcript + 本摘要。 |

`util/pe_import_inject/{inject.ps1, wuwa_patch.ps1, poc_dll/...}` 这次没改，
已经在 `89dd97255 / d8de9c137` 两个 commit 里 push 到 origin。

## 关键 bug & fix（本阶段）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | patched `Client-Win64-ShippingBase.dll` 直接双击启动游戏时弹 0xc000007b | 是 Shipping.exe **业务层**因为缺 launcher token 退出，**不是** PE format 错。Step B（绕过 launcher 直接启 Shipping.exe）证明 DllMain 跑通了，PEB.Ldr 顺序正确：`Shipping → ntdll → kernel32 → ShippingBase → TinecmaTool_PoC`。 | 无需修，本来 PE patch 路径是正确的，弹窗误导。 |
| 2 | `ilspycmd` 最新版 NuGet 包 manifest 损坏，`dotnet tool install -g ilspycmd` 失败：「NuGet 包中找不到 DotnetToolSettings.xml」 | 工具发布问题（ICSharpCode.ILSpyCmd 10.1.0.8386 package）。 | 锁版本 `--version 8.2.0.7535` 装老版本，反编译能力一样。 |
| 3 | launcher_patcher 第一次跑后用 ilspycmd 反编译 patched dll 验证，KRResCheckFlow.Exec 方法体输出 `resultCallback?.Invoke(success: true, 0, "TinecmaTool...", null);` ✅ 完全符合预期 | 一次过。 | （无） |
| 4 | `move_agent_to_root` 切到 `-threadctx-inject` workspace 失败：`git fetch refs/heads/Endfield` 在 origin 找不到 | 我当前的 Endfield 分支只在 local，没有 remote。MCP 工具默认会 fetch 当前分支。 | 不切 root，直接用绝对路径 + `git -C` 操作目标 workspace。 |

## 已知遗留 / 下一步

1. **patcher 输出能否在 launcher_updater 自检下存活**：尚未实测。launcher_main.dll
   在 `filechecklist.json` 里有 md5/size 记录，patch 后必然不匹配。如果
   launcher_updater 启动时校验 launcher_main.dll，会触发 launcher 自更新；这一
   步要在 `KRComponentUpdate.KRUpdateWorker` 里找入口并补 patch。
2. **Authenticode 签名**：原 dll 有库洛的有效代码签名（DigiCert，2025-06-18 ~
   2028-06-23），patch 后失效。.NET runtime 不强制签名，但 launcher 自己若调
   `ProcessUtils.VerifyFileCertThumbsPrint(self)` 校验自己则要绕。
3. **patched ShippingBase.dll + patched launcher_main.dll 组合下进游戏的实测**：
   - 需要进 launcher 看 UI 是否正常、"启动游戏"按钮是否可用、点击后 game 是
     否能起到登录页（不进 game 业务，只验证 patch 路径在 launcher → ACE-Base →
     Shipping 这条链上活到 DllMain）。
   - 用户在该消息发出时已经做完 Step B（绕过 launcher）验证 patch DLL 完美，
     但 Step C（带 launcher + ACE）还没跑。
4. **ACE-Base 对 PEB.Ldr 异常 dll 的反应**：本工具让 TinecmaTool_PoC.dll 作为
   ShippingBase 的合法 static import 出现在 PEB.Ldr，看起来像 ShippingBase 自
   己的依赖。ACE 是否会按 hash / path 做 module 黑白名单，目前未知，需要实
   测。
5. **没解决的安全护栏**：用户曾在 Step B 看到弹窗 "提示：检查到游戏文件缺失"
   和 "警告 (3, 1021, 17008) [c1144436cfe733bfc8ccc6]" 两个弹窗，文案在
   launcher_main 字符串里搜不到 → 大概率在 `KRLauncher.SkinRes.dll` /
   `KRLauncherUpdater.SkinRes.dll` 的 WPF resource 里（baml 资源）。这意味着
   UI 文案是从 WPF resource 来的，但触发逻辑在托管 C# 里，我们 patch C# 即可
   截断；不需要动 WPF 资源。

## 链路总览（本仓库）

```
threadctx-inject (CreateRemoteThread → SetThreadContext 改造)
   └─ manualmap-inject (LoadLibrary 仍走 ntdll 但被 ACE-Base kill)
        └─ pe-import-inject ← 当前位置
             ├─ inject.ps1            # 通用 PE patcher
             ├─ wuwa_patch.ps1        # 鸣潮专用 wrapper
             ├─ poc_dll/              # 验证 PE loader 真的把我们 load 进去
             └─ launcher_patcher/     # ★ 本阶段新增：绕过 launcher md5 校验
```

## 命令片段 cheatsheet

```powershell
# patch game DLL
.\wuwa_patch.ps1 -GameDir 'D:\Wuthering Waves\Wuthering Waves Game'

# 编 + 跑 launcher patcher
cd launcher_patcher
cp 'C:\Wuthering Waves\<version>\launcher_main.dll' ..\launcher_decompile\
dotnet run -c Release

# 安装 patched launcher_main.dll（交互式确认）
.\apply_launcher_patch.ps1 -LauncherDir 'C:\Wuthering Waves\<version>'

# 任何时候回滚
.\apply_launcher_patch.ps1 -LauncherDir 'C:\Wuthering Waves\<version>' -Restore
.\wuwa_patch.ps1 -GameDir 'D:\Wuthering Waves\Wuthering Waves Game' -Restore
```
