# TinecmaTools - RenderDoc 反作弊绕过工具集

本工具集提供了多种方法来绕过游戏反作弊系统对 RenderDoc 的检测,适用于图形开发、性能分析等合法用途。

## ⚠️ 免责声明

本工具仅供学习研究和合法的图形开发用途。使用者需自行承担使用风险,作者不对任何非法使用承担责任。

## 📦 工具列表

> 📁 **项目结构**: 完整的文件和目录说明请查看 [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md)

### 📖 文档

1. **快速开始指南** - [QUICK_START.md](QUICK_START.md)
   - 5分钟快速上手教程
   - 详细的步骤：
   
   1）先global hook 

   2）然后勾选 capture child process 

   3）launch 启动器 

   4）使用Inject到需要注入的进程 
   

   ![快速开始步骤](Zmd截图.png)

   ![Inject 步骤](Inject截图_17707116436894.png)

### 🛠️ 工具

### 1. 自动化绕过工具 ⭐ (推荐)
- **文件**: `tools/auto_bypass.py`
- **用途**: 一键执行完整的绕过流程
- **使用**:
  ```bash
  python tools/auto_bypass.py \
    --renderdoc "C:\Program Files\RenderDoc" \
    --game "C:\Games\YourGame\game.exe"
  ```

### 2. 文件重命名工具 (PowerShell)
- **文件**: `tools/rename_tool.ps1`
- **用途**: 批量重命名 RenderDoc 文件以伪装身份
- **使用**:
  ```powershell
  .\tools\rename_tool.ps1 -RenderDocPath "C:\Program Files\RenderDoc" -NewName "TinecmaTools"
  ```

### 3. DLL 注入工具 (PowerShell)
- **文件**: `tools/inject_dll.ps1`
- **用途**: 手动注入 RenderDoc DLL 到目标进程
- **使用**:
  ```powershell
  .\tools\inject_dll.ps1 -ProcessName "game.exe" -DllPath "C:\Path\To\renderdoc.dll" -Delay 5
  ```

### 4. 字符串替换工具 (Python)
- **文件**: `tools/string_replacer.py`
- **用途**: 修改二进制文件中的特征字符串
- **使用**:
  ```bash
  python tools/string_replacer.py renderdoc.dll
  ```

### 5. 进程列表 Hook (C++)
- **文件**: `tools/hook_process_list.cpp`
- **用途**: Hook 系统 API 隐藏 RenderDoc 进程
- **编译**:
  ```bash
  cl /LD tools/hook_process_list.cpp
  ```


## 🚀 快速开始

> 💡 **新手推荐**: 
> - 查看 [QUICK_START.md](QUICK_START.md) 获取详细的5分钟快速上手指南
> - 运行 `example_usage.ps1` 或 `example_usage.bat` 使用交互式菜单

### 方法一: 自动化流程(推荐)

```powershell
# 1. 确保已安装 Python 3.x
python --version

# 2. 运行自动化工具
python tools/auto_bypass.py --renderdoc "C:\Program Files\RenderDoc" --game "C:\Games\YourGame\game.exe"
```

### 方法二: 手动步骤

```powershell
# 步骤 1: 重命名文件
.\tools\rename_tool.ps1

# 步骤 2: 修改特征字符串
python tools\string_replacer.py "C:\Program Files\RenderDoc\renderdoc.dll"

# 步骤 3: 启动游戏后注入
.\tools\inject_dll.ps1 -ProcessName "game" -DllPath "C:\Program Files\RenderDoc\renderdoc.dll"
```

## 📋 检测绕过检查清单

使用前请确认:

- [ ] 已重命名所有 RenderDoc 相关文件
- [ ] 已修改二进制文件中的特征字符串
- [ ] 使用延迟注入避开启动检测
- [ ] 测试环境下验证功能正常
- [ ] 确认用途合法合规

## 🛠️ 技术原理

### 常见检测方法

1. **进程名检测**
   - 扫描进程列表
   - 匹配黑名单进程名

2. **内存特征检测**
   - 扫描已加载 DLL
   - 检测特征字符串

3. **驱动级检测**
   - 内核回调监控
   - 对象枚举

### 对应绕过方法

1. **文件伪装**
   - 重命名可执行文件
   - 修改内部字符串

2. **延迟注入**
   - 在初始化后注入
   - 避开启动检测

3. **API Hook**
   - Hook 进程枚举函数
   - 隐藏进程信息

## 📊 兼容性

| 反作弊系统 | 兼容性 | 备注 |
|-----------|--------|------|
| 基础进程检测 | ✅ 完全支持 | 重命名即可绕过 |
| EasyAntiCheat | ⚠️ 部分支持 | 需要多种方法组合 |
| BattlEye | ⚠️ 部分支持 | 需要内核级对抗 |
| Vanguard | ❌ 不支持 | 内核级反作弊,难度极高 |

## 🔧 故障排除

### 问题: 从 Git 拉取后编译报错

**解决方案**: 请查看 [编译故障排除文档](docs/BUILD_TROUBLESHOOTING.md)，涵盖：
- 缺少 `wymsrv.dll` / `symsrv.dll` / `dbghelp.dll` 的解决方法
- SWIG、Qt 自定义生成失败（退出码 9009、3）的解决方法
- 前置环境与依赖说明

### 问题: 注入失败

**解决方案**:
1. 确认游戏进程正在运行
2. 以管理员身份运行工具
3. 增加注入延迟时间

### 问题: 仍被检测

**解决方案**:
1. 检查是否修改了所有特征字符串
2. 尝试使用虚拟机隔离
3. 考虑使用内核级 Hook

### 问题: 游戏崩溃

**解决方案**:
1. 确认 RenderDoc 版本与游戏兼容
2. 尝试不同的注入时机
3. 检查是否有冲突的其他工具

## 📚 参考资料

- [RenderDoc 官方文档](https://renderdoc.org/docs/)
- [Windows API Hook 技术](https://docs.microsoft.com/en-us/windows/win32/)
- [反作弊系统分析](https://github.com/topics/anti-cheat)

## 🔒 安全提示

1. **仅用于合法目的**: 单机游戏优化、图形开发学习等
2. **风险自负**: 可能违反游戏服务条款导致封禁
3. **保护隐私**: 不要在公共平台分享具体绕过细节
4. **持续更新**: 反作弊系统会不断升级,需要持续研究

## 📝 版本历史

- **v1.0** (2026-02-08): 初始版本
  - 基础文件伪装
  - 字符串替换
  - DLL 注入
  - 自动化流程
  - 品牌名称: TinecmaTools

## 🤝 贡献

欢迎提交问题和改进建议!

## 📄 许可证

本工具集仅供学习研究使用。