# 开发运维手册（ModbusGateway）

面向"要在客户机上跑起来/排错/答辩演示"的场景。接口细节看 `docs/API.md`，架构和目录看 `README.md`。

---

## 一、快速开始

```bash
cd ~/network/lianxi          # 必须在工程目录里敲 make（在家目录 ~ 里敲会报"找不到 makefile"）

make                         # 编译：thttpd.out（web） + collector.out（采集进程）
make up                      # 一键后台起全部：Modbus 从机模拟器 + 采集守护进程 + web(8080)
make down                    # 一键停全部（连被 Ctrl+Z 挂起的、sudo 起的都会处理）
make status                  # 看采集进程状态 + 共享内存里的实时值/极值/点数/周期
make test                    # 跑全部自动化测试（121 项断言，约 1 分钟）
make clean                   # 清编译产物和运行日志
```

`make up` 跑完会直接打印浏览器地址：

```
登录/注册   http://<虚拟机IP>:8080/app/index.html     演示账号 admin / 123456
数据看板    http://<虚拟机IP>:8080/app/dashboard.html
早期监控页  http://<虚拟机IP>:8080/realtime.html
教学首页    http://<虚拟机IP>:8080/index.html
```

> **`/` 打开的是课程原始教学首页**（`wwwroot/index.html`），本工程做的 ModbusGateway 前端在
> **`/app/index.html`** —— 演示时别开错页面，否则会以为改的东西没生效。

### 手动跑（调试单个进程时用，要开两个终端标签）

```bash
python3 tools/modbus_slave.py &            # 1) Modbus TCP 从机模拟器，监听 5020
./collector.out                            # 2) 前台跑采集（调试）；常驻用 ./collector.out --daemon
./thttpd.out 8080                          # 3) 前台跑 web（终端不会回提示符，这是正常的）
./thttpd.out 8080 > logs/web.log 2>&1 &    #    或者后台跑
```

- **不带端口参数 = 监听 80**，要 sudo 密码；带 `8080` 就免 sudo。看到 `[sudo] 密码：` 别慌。
- `./thttpd.out` 是常驻进程，前台跑的时候终端不回到提示符 —— 不是卡死。想省事直接 `make up`。

---

## 二、采集进程的四种模式

```bash
./collector.out               # 前台跑（调试，日志直接打屏）
./collector.out --daemon      # 后台守护：fork+setsid，写 run/collector.pid，日志进 logs/collector.log
                             # 父进程监管子进程：子进程崩溃/kill -9 → 3 秒后自动拉起（最多 20 次）
                             # 子进程正常退出(0) → 父进程一起退
./collector.out --stop        # 按 pid 文件停止（对进程组发 SIGTERM，父子一起收）
./collector.out --status      # pid + 共享内存实时数据/极值/点数/周期 + 数据来源 + 设备目标
./collector.out --show-conf   # 打印合并后的生效配置（配置文件 + 命令行参数）
```

- **Modbus 自愈**：连续读失败 5 次 → `modbus_close` + `modbus_connect` 重连。
- **日志**：同时写 SQLite `logs` 表和 syslog（IDENT=`modbus-collector`）。modbus 报错 30 秒限流一条。
- 守护模式下会 `chdir("/")`，所以 DB 路径和日志路径在启动时就算成绝对路径。

---

## 三、测试工具

`tests/` 是自动化测试，`tools/` 是运行/调试工具，两边不要混。

| 文件 | 用途 |
|---|---|
| `tests/run_all_tests.sh` | **一键全流程**，8 个阶段，121 项断言（推荐只用这个） |
| `tests/db_selftest.c` | 数据库层自测 36 项：SHA-256 官方向量 / 注册 / 重复注册被拒 / 登录对错 / 插记录 / 按跨度查历史 / 分级日志 / 会话表 |
| `tests/tests_api_rest.py` | REST 8 接口 34 项：token 鉴权、401、注册重复、指令下发、未知接口 404 |
| `tests/tests_web.py` | HTTP 运行时 27 项：静态/登录/求和/404/405/路径穿越 4 种/404页缺失兜底/超大 Content-Length/5000字节 URL |
| `tests/test_api.py` | 老接口 + 共享内存/消息队列集成 8 项（含"两次读数在变 = 活数据"） |
| `tests/test_daemon.sh` | 采集守护进程 11 项：启动 → 状态 → `kill -9` 子进程看自动拉起 → `--stop` |
| `tests/test_session_persist.sh` | 会话持久化 5 项：重启 web 后旧 token 仍有效 |
| `tools/modbus_slave.py` | Modbus TCP 从机模拟器（纯 python3 标准库，不需要 pymodbus/diagslave），寄存器0=温度×10、寄存器1=湿度×10，值随时间漂移 |
| `tools/start_all.sh` / `stop_all.sh` | 一键启停（`make up` / `make down` 就是调它们） |
| `tools/clean_db.sh` | 清测试数据（`make clean-db` / `make db-stats`），清前自动备份到 `~/backups_lianxi/db_backups/` |
| `tools/mock_server.py` | Python 版模拟后端，只在"纯前端 MOCK 联调"时用（`config.js` 里 MOCK=true），平时不需要 |

一键测试的 8 个阶段：

```
1/8 编译              2/8 数据库层自测        3/8 起从机+采集+web
4/8 REST接口（含会话持久化）  5/8 老接口+IPC集成    6/8 web运行时
7/8 前端静态托管+数据库汇总   8/8 采集守护进程
```

当前基线：**121 PASS / 0 FAIL，编译 0 warning**（db 36 + REST 34 + 会话 5 + 老接口 8 + web 27 + 守护 11）。
跑测试时注意用**真实用户身份**（`su hq -c '...'`），root 编出来的产物会让后面的非 root 步骤权限不够。

---

## 四、环境依赖（已装好；换机器/重装系统要重建）

| 依赖 | 位置/做法 |
|---|---|
| cJSON 1.7.18 | **18.04 的 apt 源里没有 libcjson-dev**，只能源码编译装到 `/usr/local`：头 `/usr/local/include/cjson/cJSON.h`、库 `/usr/local/lib/libcjson.so`，装完 `ldconfig` |
| libmodbus | 头文件是扁平的 `/usr/include/modbus.h`（代码里写 `<modbus.h>`）；另有 shim `/usr/include/libmodbus/libmodbus.h` 兜底 |
| `-lrt` | 18.04 是 glibc 2.27，`mq_open` 在 librt 里，Makefile 必须带 `-lrt` |
| `-I. -Isrc` | 引号 include 以源文件所在目录为基准；`-Isrc` 让 `src/http/*.c` 能写 `#include "ipc/shm.h"` |
| 密码哈希 | 自带纯 C SHA-256（`src/db/db.c`），不依赖 openssl |
| 客户机其它 | python3 是 **3.6**（没有 `ThreadingHTTPServer`，`tools/mock_server.py` 已做兼容）；**没有 curl**，用 python3 urllib/裸 socket；**没装 git** |

---

## 五、设备配置（接真实设备只改配置，不改代码、不用重编译）

配置全在工程根的 `collector.conf`（key=value，`#` 注释支持行内写法），命令行参数可逐项覆盖。
看当前生效配置：`./collector.out --show-conf`

```bash
# 网络型温湿度计（Modbus TCP）
./collector.out --host 192.168.1.100 --port 502 --slave 1 --real --source "车间1号温湿度计" --daemon

# RS485 串口设备（Modbus RTU）
./collector.out --transport rtu --serial /dev/ttyUSB0 --baud 9600 --slave 1 --real --daemon
```

| 配置项 | 含义 | 默认 |
|---|---|---|
| transport | tcp / rtu | tcp |
| host / port | 设备 IP 与端口（标准 Modbus TCP 是 502） | 127.0.0.1 / 5020 |
| slave | 从机地址（设备手册的站号） | 1 |
| serial / baud / parity | RTU 串口参数 | /dev/ttyUSB0 / 9600 / N |
| reg_temp / reg_humi | 温度、湿度寄存器地址 | 0 / 1 |
| scale_temp / scale_humi | 真实值 = 寄存器原始值 ÷ 该值 | 10 / 10 |
| interval_ms / threshold | 采集周期 / 温度告警阈值 | 2000 / 32.0 |
| source_name / source_simulated | 数据来源标记 | modbus-simulator / 1 |

**数据来源标记**会显示在三个地方：网页看板顶部橙色横幅 + "系统运行状态"里的"数据来源"行、
`./collector.out --status`、SQLite `logs` 表 + syslog（启动时写一条）。接真实设备后设 `source_simulated=0`。

> 配置文件解析会**剥掉行内注释**：`host = 127.0.0.1  # 注释` 是合法的；但别把注释写在值中间。

---

## 六、常见故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `bind failed: Address already in use` | 之前的 thttpd 没退干净，**尤其是被 Ctrl+Z 挂起的**（SIGTSTP 只暂停、端口还占着、SIGTERM 对它无效）。先 `bash tools/stop_all.sh`，或 `kill -CONT <pid>`，或 `sudo kill -9 <pid>`。换端口：`bash tools/start_all.sh 8081` |
| 前台跑 web 后终端不回来 | 正常，服务是常驻进程。用 `make up` 或开第二个终端标签 |
| 提示要 sudo 密码 | 没带端口参数 → 默认监听 80。改成 `./thttpd.out 8080` |
| 日志里 `recv:GET /...` 一大段 | 代码里 `#if 1` 故意打印的原始报文，不是错误 |
| 日志里 `path = wwwroot/favicon.ico` + `can't find file` | 浏览器来要标签页图标。工程已放 `wwwroot/favicon.ico` 消除 |
| 终端显示不出 `℃` | 等宽字体缺字形，程序输出本身没问题（换 DejaVu Sans Mono 或写成 `C`） |
| `make` 说"对all无需做任何事" | 增量构建已最新，不是错误 |
| `cjson/cJSON.h: 没有那个文件` | 系统里 cJSON 没装到 `/usr/local`（见第四节） |
| `undefined reference to mq_open` | Makefile 少了 `-lrt` |
| 服务起来了但页面打不开 | 先 `pgrep -f thttpd.out` + `ss -lntp \| grep 端口` 判断进程和端口，别只靠页面；静态文件探测用**原始 socket**（本服务器的 200 响应没有 Content-Length，urllib 容易误报异常） |
| 数据库里看不到新数据 | 隔 5 秒数两次 `SELECT COUNT(*) FROM sensor_data`，行数在涨=采集+入库正常 |
| 感觉"改了代码没生效" | 十有八九是编辑器打开的是别的目录（备份目录/`thttpd-master`）。工程唯一工作目录是 `~/network/lianxi`；还不行就在编辑器里 Ctrl+Shift+P → **Revert File**（旧缓冲区保存会把改动盖回去） |

---

## 七、还没做 / 后续可做

- epoll reactor（现在是 accept + 每连接一线程）
- `/api/command` 的 `restart` 只在日志里记一条，不会真的重启采集进程
- 日志只落 SQLite + syslog，未做按天轮转与压缩
- 前端"记住我"、修改密码、用户管理页未实现
- 两套用户体系没合并：教学页 `POST /login` 仍是硬编码 admin/admin，新的 `/api/login` 才走数据库
- 密码是明文经 HTTP 传输（教学环境未上 TLS）
- `sensor_data.timestamp` 存的是 SQLite **UTC**（客户机时区是 America/Los_Angeles）→ 前端时间显示会有偏差

---

## 八、目录与备份约定

- 正式工程（唯一工作目录）：`/home/hq/network/lianxi`
- 客户机上还有 `/home/hq/network/thttpd-master`（课程原始素材）、
  `/home/hq/network/linux/lianxi`（早期 C 练习，**不是本工程副本**）、`/home/hq/network/wangbian`
- 备份统一放 `/home/hq/backups_lianxi/`（**不在工程同级**），里面有各版快照 + `db_backups/` + `README_总说明.txt`
- 工程内 `sensor.db.bak_*` 不再生成（`tools/clean_db.sh` 直接备份到 `~/backups_lianxi/db_backups/`）
- 生成物（`*.out`、`sensor.db*`、`logs/`、`run/`）见 `.gitignore`
