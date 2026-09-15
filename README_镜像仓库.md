# lianxi 工程 —— 主机侧 git 镜像仓库

## 这是什么

工程的真身在虚拟机里：**`/home/hq/network/lianxi`**（VMware 那台 Ubuntu 18.04，VMX：
`F:\华清远见\18jiaoxue-2024.6.26\Ubuntu 64 位.vmx`）。

**客户机里没装 git，也没有外网**，所以工程本身没法做版本管理，而且它只存在于虚拟机里
（VM 坏了/删了就全没了）。于是这里放一个**主机侧的镜像仓库**：每次在客户机改完代码，
在主机上跑一下 `_sync_from_vm.sh`，就把客户机的最新代码拉下来提交一次 —— 既是版本历史，
也是异地备份。

> 所以：**改代码永远在客户机里改**（VS Code / vim），这个仓库只读不写，只用来留档和回溯。

## 怎么用

在 git-bash 里：

```bash
cd /c/Users/Lenovo/source/repos/ModbusGateway
bash _sync_from_vm.sh "这次改了什么"      # 不带说明就用时间戳
```

前置条件：VMware 里那台 Ubuntu 客户机**开机运行中**（不需要客户机有网 —— 走的是 VMware Tools
通道，客户机网卡断了照样能同步）。

看历史 / 对比改动：

```bash
git log --oneline --stat
git diff HEAD~1 -- src/http/thttpd.c
git show 1d9363d                 # 看某次整理都动了什么
```

## 现有历史

| 提交 | 内容 |
|---|---|
| 初始快照 | 整理前的工程（手写 HTTP 服务器 thttpd + sqlite/cJSON + 共享内存/消息队列 + Modbus 采集） |
| 工程清理 | 权限归一、删死代码（HTML_HEAD / NORMAL / WRONING / FATAL）、文档与现状对齐、加 `.gitignore`、数据库备份移出工程目录 |
| 源码分层重构 | 源码全部收进 `src/`（`http/ ipc/ db/ collector/` + `main.c`）、测试拆到 `tests/`、文档归到 `docs/`、Makefile 改用 `-Isrc` |
| 同步脚本+说明 | 本文件与 `_sync_from_vm.sh`（这两个是本仓库自己的文件，客户机工程里没有） |

## 客户机那边的备份

除了这个仓库，客户机里还有一套完整快照，在 `/home/hq/backups_lianxi/`：

- `lianxi_backup_20260914_0410` —— 最早（修复前）
- `lianxi_backup_before_webapi` —— 接入 REST 接口之前
- `lianxi_backup_20260914_1745` —— 整理前
- `lianxi_backup_20260914_1755_before_srcmove` —— 源码分层重构前
- `db_backups/` —— sensor.db 历史备份
- `README_总说明.txt` —— 每一版是什么 + 回退命令

## 想推到 GitHub 的话

工程里**没有真实姓名/密码之类的敏感信息**（演示账号是 admin/123456），可以直接：

```bash
git remote add origin git@github.com:<你的账号>/<仓库名>.git
git push -u origin main
```

注意 `.gitignore` 已经排除了编译产物、`sensor.db*`、`logs/`、`run/`，推送前 `git status` 确认一下没有运行时数据混进来。
