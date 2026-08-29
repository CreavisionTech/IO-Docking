# iodock · IO Dock 控制 Skill

让 Claude Code 通过一个命令行工具直接操作 IO Dock 板卡。

## 文件

- `iodock.py` — 命令行工具(纯 Python,pyserial,零其它依赖;走板卡文本协议)
- `SKILL.md` — Claude Code skill 说明(描述板卡能力与用法)

## 用法(仓库内)

在仓库根目录运行:

```bash
pip install pyserial          # 一次性
python skills/iodock/iodock.py ping
python skills/iodock/iodock.py io write 1 high
python skills/iodock/iodock.py pwm cfg 1 1000 50
```

端口自动识别(VID:PID=1209:8888);识别不到时 `IO_DOCK_PORT=COMxx`。

## 安装为 Claude Code Skill

**项目级(仅本仓库)** —— 复制到仓库根目录 `.claude/skills/` 即可自动发现:

```bash
mkdir -p .claude/skills && cp -r skills/iodock .claude/skills/
```

**用户级(所有项目可用)** —— 复制到 `~/.claude/skills/`:

```bash
mkdir -p ~/.claude/skills && cp -r skills/iodock ~/.claude/skills/
```

安装后,Claude Code 处理涉及 IO Dock 的任务时会自动加载 `SKILL.md`,并通过 `iodock` 命令操作板卡。

> 依赖:`pyserial`(仅此一个)。若不想装 Python 环境,可把 `iodock.py` 打包成单文件 exe 后替换即可。
