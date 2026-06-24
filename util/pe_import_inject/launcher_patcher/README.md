# `launcher_patcher` — Wuwa launcher md5 校验绕过

把鸣潮 launcher（`launcher_main.dll`）里 `KRResources.KRResCheckFlow.Exec()`
方法体替换成单一 `resultCallback?.Invoke(success: true, ...)` 调用，让 launcher
启动时不再去算游戏文件 md5/size，自然就不会因为我们 patch 过的
`Client-Win64-ShippingBase.dll` 而弹"游戏文件缺失"。

## 为什么需要它

只 patch `Client-Win64-ShippingBase.dll` 让 PE loader 把 `TinecmaTool*.dll` 当
静态依赖加载——这一步在 `Client-Win64-Shipping.exe` 直接运行时**完美工作**
（DllMain 跑通、PEB.Ldr 顺序正确）。但走 launcher 启动游戏时 launcher 会先做
md5 完整性校验，导致游戏根本起不来。

校验入口（从反编译 `launcher_main.dll` 看到）：

```
[WebView2 H5 前端]
   ↓ window.callClient("kr_check_res_valid", ...)
KRWebViewVM.HandleCheckResValid
   ↓
KRWebViewController.OnCheckResValid
   ↓
KRResUpdateModule.CheckResValid
   ↓
KRResCheckFlow.Exec                    ← patcher 改这里
   ↓ (本来会创建 KRFileChunkCheckTask)
KRFileChunkCheckTask.Run               ← 真正算 md5/size 的地方
```

把 `KRResCheckFlow.Exec` 的方法体整个换成一句 callback success，下游所有
md5/size/modifyTime 校验都不会发生。

## 依赖

- .NET 8 或 .NET 9 SDK（项目 target `net9.0`，但 dnlib 兼容到 .NET Standard 2.0
  所以 net8.0 也能跑——改 `LauncherPatcher.csproj` 的 `<TargetFramework>` 即可）
- [`dnlib`](https://www.nuget.org/packages/dnlib) 4.4.0+（NuGet 自动还原）

## 输入 / 输出

- **输入**：你自己从 `C:\Wuthering Waves\<version>\launcher_main.dll` 复制过来
  的 launcher dll。**仓库不带这个文件**（库洛专有 binary）。
- **输出**：同名加 `.patched` 后缀的 dll，例如
  `launcher_main.patched.dll`。

## 使用

```powershell
cd util\pe_import_inject\launcher_patcher

# 1. 准备输入 dll（默认从 ../launcher_decompile/ 读）
mkdir ..\launcher_decompile -Force | Out-Null
cp 'C:\Wuthering Waves\<version>\launcher_main.dll' ..\launcher_decompile\

# 2. 编译并跑 patcher
dotnet run -c Release
# 期望输出：
#   [*] input  : ...\launcher_decompile\launcher_main.dll
#   [*] output : ...\launcher_decompile\launcher_main.patched.dll
#   [*] target : System.Void KRResources.KRResCheckFlow::Exec(...)
#   [+] KRResCheckFlow.Exec body rewritten (9 instructions)
#   [+] wrote patched assembly (1 method(s)).

# 3. 安装到 launcher 目录（自动备份原 dll，等你确认才保留）
.\apply_launcher_patch.ps1 -LauncherDir 'C:\Wuthering Waves\<version>'

# 4. 启动 launcher 看效果。如果出问题，回到 step 3 的窗口直接回车，自动 rollback。
#    任何时候都可以手动还原：
.\apply_launcher_patch.ps1 -LauncherDir 'C:\Wuthering Waves\<version>' -Restore
.\apply_launcher_patch.ps1 -LauncherDir 'C:\Wuthering Waves\<version>' -Status
```

## 自定义 input / output 路径

```powershell
dotnet run -c Release -- 'D:\src\launcher_main.dll' 'D:\out\launcher_main.patched.dll'
```

## 验证 patch 生效

```powershell
# 用 ilspycmd 反编译 patched dll，看 KRResCheckFlow.Exec 的源代码
dotnet tool install -g ilspycmd --version 8.2.0.7535
ilspycmd -p -o "$env:TEMP\verify" ..\launcher_decompile\launcher_main.patched.dll
gc "$env:TEMP\verify\KRResources\KRResCheckFlow.cs" | sls "Exec\(" -Context 0,5
# 期望：方法体只剩一行 resultCallback?.Invoke(success: true, 0, "TinecmaTool: ...", null);
```

## 已知问题

1. **launcher 自校验**：`filechecklist.json` 记录了 `launcher_main.dll` 的
   md5/size，launcher_updater 启动时可能校验自己。如果出现"launcher 需要更新 /
   launcher 版本异常"弹窗，下一步要 patch `launcher_updater.dll` 跳过 self
   integrity check。
2. **Authenticode 签名**：原 dll 有库洛的有效代码签名，patch 后失效。.NET
   runtime 不强制要求托管 dll 签名所以能正常加载，但如果 launcher 自己代码
   里调 `ProcessUtils.VerifyFileCertThumbsPrint(self)` 校验自己则要绕。
3. **PE Import 注入校验**：本工具只解决 launcher 端的 md5 校验。game 主体
   `Client-Win64-Shipping.exe` 自己是否会跑运行时校验（罕见）需另测。
4. **服务端 manifest**：launcher 启动时会从服务端拉 `LocalGameResources.json`
   覆盖本地。这没关系——我们 patch 的是 launcher 拿到 manifest *之后* 做的
   md5 比对逻辑，跟 manifest 内容无关。

## 不会做的事

- ❌ 把 `launcher_main.dll` / `launcher_main.patched.dll` / 反编译源码 commit
  进仓库（版权问题）
- ❌ 自动从网上下载 launcher dll（你必须自己有合法授权的鸣潮安装）
- ❌ 任何形式的反作弊绕过（patch 仅影响 launcher UI 校验流程，不动 ACE-Base
  内核驱动或 game 业务逻辑）
