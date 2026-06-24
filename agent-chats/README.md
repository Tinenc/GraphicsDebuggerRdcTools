# agent-chats

本目录归档与 Cursor agent 协作开发 `TinecmaTool` 时的关键对话记录，方便日后查阅 / 排错追溯。

约定：

- 文件命名：`YYYY-MM-DD-<topic>-<short-uuid>.jsonl`（jsonl 原始记录）+ 同名 `.md`（压缩后的人读摘要）。
- jsonl 是 Cursor 的原始 transcript 格式，里面是逐条 user/assistant 消息事件；可用 `jq` / 文本编辑器查看。
- md 摘要只保留：**需求 → 决策 → 改动文件 → 关键 bug & fix → 已知遗留**，避免去翻几百 KB 的 jsonl。
- 不要在这里塞密钥 / 私人邮箱 / 临时 session token。如对话里有，归档前先 sanitize。
- 同一个 chat 跨主题（例如本仓库 `611501bb` 这个 chat 跨越 manual-map → PE import → launcher patcher 三个阶段）按阶段独立归档，jsonl 是 superset，md 是阶段差量。

当前归档：

| 日期 | 主题 | 对应分支 / 文档 |
|---|---|---|
| 2026-06-24 | manual-map DLL 注入（绕开 ACE-Base） | `tinecmatool/manualmap-inject`、`TINECMATOOL_MANUALMAP_INJECT.md` |
| 2026-06-25 | PE Import Table 静态注入 + 鸣潮 launcher md5 校验绕过 | `tinecmatool/pe-import-inject`、`util/pe_import_inject/README.md`、`util/pe_import_inject/launcher_patcher/README.md` |
