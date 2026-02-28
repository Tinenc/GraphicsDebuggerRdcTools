# RenderDoc / TinecmaTools 编译故障排除

从 Git 拉取后编译失败时，请按以下步骤排查。

---

## 错误 1: 找不到 wymsrv.dll / symsrv.dll / dbghelp.dll (MSB303)

**现象**: 无法复制 `renderdoc\3rdparty\dbghelp\x64\wymsrv.dll`（或 symsrv.dll、dbghelp.dll），找不到指定文件。

**原因**: `dbghelp` 目录下缺少预编译的 DLL，这些文件来自 Windows Debugging Tools，未包含在 Git 仓库中。

**解决步骤**:

1. 安装 **Windows SDK** 或 **Debugging Tools for Windows**：
   - 下载: https://developer.microsoft.com/windows/downloads/windows-sdk
   - 安装时勾选 **Debugging Tools for Windows**

2. 找到 DLL 所在目录（通常在）：
   - `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\` → 复制 `dbghelp.dll`、`symsrv.dll`、`symsrv.yes`
   - `C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\` → 同上

3. 创建目录并复制文件：
   ```
   renderdoc\3rdparty\dbghelp\x64\   ← 放入 x64 版本的 3 个文件
   renderdoc\3rdparty\dbghelp\x86\   ← 放入 x86 版本的 3 个文件（如需要 32 位构建）
   ```

4. 若 `symsrv.yes` 不存在，可从 `symsrv.dll` 同目录查找，或从已安装的 RenderDoc 中复制。

---

## 错误 2: renderdoc.i / qrenderdoc.prenderdoc 自定义生成退出代码 9009 (MSB8006)

**现象**: `renderdoc.i` 或 `qrenderdoc.prenderdoc` 的自定义生成已退出，代码为 9009。

**原因**: 退出码 9009 表示「命令未找到」。构建需要 **SWIG** 和 **Qt** 工具（moc、uic），这些通常来自 `qrenderdoc_3rdparty.zip`，Git 仓库中未包含。

**解决步骤**:

1. 下载 RenderDoc 官方第三方依赖包：
   - 地址: https://renderdoc.org/qrenderdoc_3rdparty.zip
   - 若该链接不可用，可尝试: https://renderdoc.org/builds 页面中的说明或镜像

2. 解压到 `qrenderdoc` 目录：
   - 将 zip 内的 `3rdparty` 文件夹**合并**到 `qrenderdoc\3rdparty\` 中
   - 确保存在：
     - `qrenderdoc\3rdparty\swig\swig.exe`
     - `qrenderdoc\3rdparty\qt\x64\bin\moc.exe` 和 `uic.exe`（或 `Win32` 对应路径）

3. 若无法获取上述 zip，可手动配置：
   - **SWIG**: 安装 SWIG 并加入 PATH，或将 `swig.exe` 放到 `qrenderdoc\3rdparty\swig\`
   - **Qt**: 安装 Qt 5.x，设置环境变量：
     - `RENDERDOC_QT_PREFIX64` = Qt 64 位安装路径（如 `C:\Qt\5.15.2\msvc2019_64`）
     - `RENDERDOC_QT_PREFIX32` = Qt 32 位安装路径（如需 32 位构建）

---

## 错误 3: .ui 文件自定义生成退出代码 3 (MSB8006)

**现象**: `PerformanceCounterSelection.ui` 及多个 `.ui` 文件的自定义生成退出，代码为 3。

**原因**: Qt 的 `uic.exe`（User Interface Compiler）未找到或执行失败，同样依赖 `qrenderdoc_3rdparty.zip` 中的 Qt 工具。

**解决步骤**:

1. 按 **错误 2** 的方式获取并解压 `qrenderdoc_3rdparty.zip` 到 `qrenderdoc\3rdparty\`。

2. 确认存在：
   - `qrenderdoc\3rdparty\qt\x64\bin\uic.exe`
   - `qrenderdoc\3rdparty\qt\x64\bin\moc.exe`

3. 若使用系统安装的 Qt，确保 `RENDERDOC_QT_PREFIX64` 指向的目录下 `bin` 中有 `uic.exe` 和 `moc.exe`。

---

## 前置环境检查清单

编译前请确认：

- [ ] **Visual Studio 2019/2022**（含 C++ 桌面开发工作负载）
- [ ] **Windows SDK**（任意版本，建议 10.0 以上）
- [ ] **Debugging Tools for Windows**（用于 dbghelp/symsrv）
- [ ] **qrenderdoc_3rdparty.zip** 已解压到 `qrenderdoc\3rdparty\`（含 Qt、SWIG、PySide2 等）

---

## 若仍无法编译

1. **使用预编译版本**：从 https://renderdoc.org/builds 下载官方构建，再使用本项目的 Python/PowerShell 工具进行反作弊绕过，无需自行编译 RenderDoc。

2. **仅构建核心库**：若只需 `renderdoc.dll` 等核心组件，可尝试在解决方案中排除 `qrenderdoc`、`pyrenderdoc_module` 等 UI/Python 相关项目，先解决 dbghelp 问题后再逐步启用。

3. **参考官方文档**：https://github.com/baldurk/renderdoc/blob/v1.x/docs/CONTRIBUTING/Compiling.md
