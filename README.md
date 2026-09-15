# ModbusGateway —— 工业物联网温湿度采集监控平台

手写 HTTP 服务器 + 多进程 IPC + Modbus TCP 采集 + Web 监控看板。
纯 C 实现（除前端和测试脚本），不依赖任何 Web 框架。

## 一、系统架构

```
   浏览器 (注册/登录 + ECharts 看板)          wwwroot/app/
        │  HTTP/1.1
        ▼
   ┌─────────────────────────────────────────────┐
   │ webserver 进程  thttpd.out                  │
   │  · 手写 HTTP 解析（多线程，每连接一线程）    │
   │  · 静态文件托管 wwwroot/（含前端页面）       │
   │  · RESTful 接口 /api/xxx（api_rest.c）      │
   │  · 用户系统：注册/登录/token 会话            │
   └───────┬──────────────┬──────────────┬───────┘
           │ 读共享内存    │ 投消息队列    │ 读写SQLite
           ▼  (读写锁)     ▼ (POSIX mq)   ▼
   ┌──────────────────┐ ┌──────────────────┐ ┌──────────────┐
   │ 共享内存          │ │ 消息队列          │ │ SQLite3      │
   │ 实时温湿度        │ │ 指令下发/回值     │ │ sensor_data  │
   │ +进程间读写锁     │ │                  │ │ users/logs   │
   └────────▲─────────┘ └────────▲─────────┘ └──────▲───────┘
            │ 写共享内存          │ 收指令            │ 写历史/日志
   ┌────────┴────────────────────────────────────────┴───────┐
   │ 采集进程（守护进程）  collector.out                      │
   │  父进程：fork+setsid 脱离终端、监管子进程、异常自动拉起   │
   │  子进程：周期采集、超阈值告警、断线重连、写 syslog       │
   └──────────────────────┬──────────────────────────────────┘
                          │ Modbus TCP (5020)
                          ▼
              从机设备 / 模拟器 tools/modbus_slave.py
```

## 二、目录结构

```
lianxi/
├── src/                            C 源码（全部按层分目录）
│   ├── main.c                      程序入口（accept 循环 + 每连接一个线程）
│   ├── http/                       手写 HTTP 服务器 + 业务层
│   │   ├── thttpd.c/.h             解析、路由、静态文件、错误页
│   │   ├── custom_handle.c/.h      业务分发：表单登录/求和、老接口 /api?cmd=
│   │   └── api_rest.c/.h           RESTful 接口层（前端用的 8 个接口 + token 会话）
│   ├── ipc/shm.c/.h                共享内存（进程间读写锁 + 结构体版本校验）
│   ├── ipc/mq.c/.h                 POSIX 消息队列（指令下发）
│   ├── db/db.c/.h                  SQLite 封装（用户/历史/日志 + 纯C SHA-256）
│   └── collector/modbus_collector.c 采集进程（守护模式 + 异常自愈 + syslog）
├── tests/                          自动化测试（121 项断言，见第六节）
│   ├── run_all_tests.sh            一键全流程（8 个阶段）
│   ├── db_selftest.c               数据库层 36 项
│   ├── tests_api_rest.py           REST 接口 34 项
│   ├── tests_web.py                HTTP 运行时 27 项
│   ├── test_api.py                 老接口 + IPC 集成 8 项
│   ├── test_daemon.sh              采集守护进程 11 项
│   └── test_session_persist.sh     会话持久化 5 项
├── tools/                          运行与调试工具
│   ├── modbus_slave.py             Modbus TCP 从机模拟器（纯标准库）
│   ├── start_all.sh / stop_all.sh  一键起停全部服务（make up / make down）
│   ├── clean_db.sh                 清理测试数据（make clean-db）
│   └── mock_server.py              Python 版模拟后端（前端 MOCK 联调用，非必需）
├── docs/
│   ├── API.md                      接口契约（REST 8 接口 + 老接口 + 错误码 + IPC 指令码）
│   └── GUIDE.md                    开发运维手册（启动排错/工具用法/环境依赖/配置项）
├── db/init.sql                     建表脚本（users / sensor_data / logs / sessions）
├── wwwroot/                        静态资源根
│   ├── app/                        ModbusGateway 前端（登录页 + 数据看板）
│   ├── realtime.html               早期实时监控页（用 /api?cmd= 老接口）
│   └── index.html login.html post.html 404.html   教学页面
├── collector.conf                  采集设备配置（接真实设备只改这里）
├── Makefile                        编译 / 运行 / 测试 / 清理入口
├── .gitignore                      生成物清单（编译产物、sensor.db、logs/、run/）
└── README.md
```

编译产物（`thttpd.out`、`collector.out`）和运行时数据（`sensor.db*`、`logs/`、`run/`）都生成在工程根，
已经在 `.gitignore` 里列出，不要手动删 `logs/`（服务在跑时删掉它会让日志写不进去）。

## 三、文档索引

| 文件 | 内容 |
|---|---|
| `README.md`（本文） | 架构、目录、编译运行、技术栈、已知限制 |
| `docs/API.md` | 全部接口契约：REST 8 个接口 + 老接口 + 错误码 + 消息队列指令码 |
| `docs/GUIDE.md` | 开发运维手册：启动/排错、测试工具用法、环境依赖、collector.conf 全表 |

## 四、编译与运行

```bash
make                 # 编译出 thttpd.out 和 collector.out

# 1) 起 Modbus 从机模拟器（没有真实设备时用，监听 5020）
python3 tools/modbus_slave.py &

# 2) 起采集进程：两种方式
./collector.out                  # 前台调试
./collector.out --daemon         # 后台守护（推荐）
./collector.out --status         # 看状态
./collector.out --stop           # 停止

# 3) 起 web 服务
./thttpd.out 8080

# 浏览器访问
#   登录/注册 : http://<IP>:8080/app/index.html     演示账号 admin / 123456
#   数据看板 : http://<IP>:8080/app/dashboard.html
#   早期监控页: http://<IP>:8080/realtime.html
```

一键起停全部服务：`make up` / `make down`（后台跑，不占终端）；一键跑全部自动化测试：`make test`（121 项断言）

## 五、接口清单

> 完整契约（错误码、指令参数、老接口、消息队列指令码）见 **`docs/API.md`**。

### RESTful（前端用，统一响应 `{"code":0,"message":"ok","data":{...}}`）
除注册/登录外都需要请求头 `Authorization: Bearer <token>`，否则返回 HTTP 401。

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/register | 注册，`{username,password}`；1002 已存在 / 1003 参数为空 |
| POST | /api/login | 登录，返回 `{username,role,uid,token}`；1001 用户名或密码错误 |
| POST | /api/logout | 退出，token 立即失效 |
| GET | /api/realtime | 实时温湿度 `{temp,humi,tempMax,tempMin,count,slaveId,online,collectTime,period,threshold}` |
| GET | /api/history?hours=24 | 历史 `{hours,limit,list:[{time,temp,humi}]}` |
| POST | /api/command | 指令下发，见下 |
| GET | /api/status | 进程状态 `{collectOnline,webOnline,modbusOnline,period,uptime}` |
| GET | /api/logs | 分级日志 `{list:[{time,level,module,message}]}` |

指令下发 `POST /api/command`：

```json
{"action":"setPeriod","period":2000}        // 改采集周期 100~60000ms
{"action":"setThreshold","threshold":32}    // 改温度告警阈值 ℃
{"action":"readRegister","addr":0,"count":2}// 读保持寄存器并取回值
{"action":"restart"}                        // 重启（由守护进程负责）
```

### 早期接口（返回裸 JSON，realtime.html 和命令行用）
`GET /api?cmd=realtime` ／ `?cmd=interval&sec=3` ／ `?cmd=history&limit=10`

### 教学接口
`POST /login`（表单登录返回跳转JS）、`POST /add`（`"data1=1data2=2"` 求和）

## 六、数据库表

| 表 | 用途 | 关键字段 |
|---|---|---|
| sensor_data | 采集历史 | temperature, humidity, device_status(0离线/1在线/2故障), timestamp |
| users | 用户 | username(唯一), password_hash(`salt$sha256(salt+password)`), created_at |
| logs | 分级日志 | time, level(DEBUG/INFO/WARN/ERROR), module, message |

## 七、IPC 约定

共享内存 `shm_sensor_data_t`（`src/ipc/shm.h`）：实时值 + 运行统计 + `pthread_rwlock_t` 进程间读写锁 +
`SHM_MAGIC` 版本校验。采集进程持写锁更新，web 端持读锁拷贝；两边锁都带超时，拿不到就报错，不会死锁。

消息队列 `/modbus_sensor_mq`（`src/ipc/mq.h`）：

| 指令 | param 含义 |
|---|---|
| CMD_QUERY (1) | 高16位=寄存器起始地址，低16位=个数 |
| CMD_SET_INTERVAL (2) | 采集周期 ms |
| CMD_SET_THRESHOLD (3) | 阈值×100（3250 → 32.5℃） |
| CMD_RESTART (4) | 无 |

## 八、对接真实设备（只改配置，不改代码、不用重编译）

设备参数全在 `collector.conf` 里，命令行参数可以覆盖它。查看当前生效配置：

```bash
./collector.out --show-conf
```

接一台网络型温湿度传感器（Modbus TCP）：

```bash
# 方式一：改 collector.conf 里的 host/port/slave/reg_temp/reg_humi/scale_*，然后
./collector.out --daemon

# 方式二：命令行直接给（示例：设备 192.168.1.100:502，站号1，温度为寄存器0÷10）
./collector.out --host 192.168.1.100 --port 502 --slave 1 \
                --reg-temp 0 --scale-temp 10 --real --source "车间1号温湿度计" --daemon
```

接 RS485 串口设备（Modbus RTU）：

```bash
./collector.out --transport rtu --serial /dev/ttyUSB0 --baud 9600 --slave 1 --real --daemon
```

`collector.conf` 的主要配置项（`transport / host / port / slave / serial / baud / reg_temp / reg_humi /
scale_temp / scale_humi / interval_ms / threshold / source_name / source_simulated`）**完整表见
`docs/GUIDE.md` 第五节**。

## 九、技术栈

**语言/标准**：C（C99/GNU11）、POSIX API、HTML5 + 原生 JavaScript（ES5）+ CSS3

**系统编程**：多线程（pthread）、进程间通信（System V 共享内存 + POSIX 消息队列 + 进程间读写锁）、
信号处理、daemon 化（fork + setsid + 二次 fork + pid 文件）、syslog 分级日志

**网络**：TCP/IP、手写 HTTP/1.0、HTTP/1.1 解析（请求行/请求头/请求体、Content-Length、Authorization）、
RESTful 接口设计、Modbus TCP（libmodbus 客户端）

**数据库**：SQLite3（WAL 模式、参数化预编译语句、并发读写、时间窗口查询）

**安全**：SHA-256 加盐口令哈希（纯 C 自实现）、token 会话鉴权、参数化 SQL 防注入、
静态资源路径穿越防护、请求体长度夹紧防栈溢出

**第三方库**：cJSON（JSON 解析/生成）、libmodbus（Modbus 协议栈）、pthread、librt

**前端**：原生 JS + ECharts 5（实时曲线/历史曲线）、fetch + Bearer token、Session/localStorage

**工具链**：GCC 7.5 + GNU Make（`-Wall -Wextra` 零警告）、Python3（联调模拟后端 + 测试脚本，
只用标准库 urllib/socket/sqlite3/json）、Ubuntu 18.04（glibc 2.27）

**测试**：121 项自动化断言、0 warning（数据库层 36、REST 34、会话持久化 5、老接口+IPC 8、
HTTP 运行时 27、采集守护进程 11），外加 SHA-256 官方向量校验与 Modbus 从机模拟器，入口 `tests/run_all_tests.sh`

## 十、已知限制 / 后续可做

- web 服务器是"每连接一线程"，未做 epoll 事件驱动（架构图里的 reactor 属下一阶段）
- 采集守护进程只做"崩溃拉起"，没有独立的看门狗进程与邮件/钉钉告警
- 日志只落 SQLite 表 + syslog，未做按天轮转与压缩
- 前端"记住我"、修改密码、用户管理页未实现
- 密码是明文经 HTTP 传输（教学环境未上 TLS）

## 十一、工程整理与版本管理（2026-09 整理）

- 目录按层归位：源码全在 `src/`（`http/ ipc/ db/ collector/` + `main.c`）、自动化测试在 `tests/`、
  运行调试工具在 `tools/`、文档在 `docs/`；生成物（`*.out`、`sensor.db*`、`logs/`、`run/`）都列进了 `.gitignore`
- 一键测试：`make test`（121 项断言全绿，编译 0 warning）；一键起停：`make up` / `make down`
- **客户机里没有 git、也没有外网**，所以版本历史维护在主机侧的镜像仓库
  `C:\Users\Lenovo\source\repos\ModbusGateway`：在主机 git-bash 里跑 `bash _sync_from_vm.sh "说明"`
  就能把客户机最新代码拉下来提交（走 VMware Tools 通道，客户机没网也照样能用）
- 客户机侧的完整快照统一放在 `~/backups_lianxi/`（各版备份 + `db_backups/` + 回退说明）
