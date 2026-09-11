# agent-chats

本目录归档与 Cursor agent 协作开发时的关键对话记录，方便日后查阅 / 排错追溯。

约定：

- 文件命名：`YYYY-MM-DD-<topic>-<short-uuid>.jsonl`（jsonl 原始记录）+ 同名 `.md`（压缩后的人读摘要）。
- jsonl 是 Cursor 的原始 transcript 格式；可用文本编辑器查看。
- md 摘要只保留：**需求 → 决策 → 改动文件 → 关键 bug & fix → 已知遗留**。
- 不要在这里塞密钥 / 私人邮箱 / 临时 session token。

当前归档：

| 日期 | 主题 | 对应分支 |
|---|---|---|
| 2026-09-12 | MuMu 模拟器捕获（Launch/GlobalHook 指向 Headless） | `tinecmatool/mumu` |
