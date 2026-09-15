# ModbusGateway 架构与代码导读

> 这份文档的目标：**让你看完能自己讲清这个项目、也能自己动手改**。
> 不是接口手册（那是 `docs/API.md`），也不是操作手册（那是 `docs/GUIDE.md`），
> 而是"数据怎么流、函数怎么调、为什么这么设计"。
>
> 所有行号都对应当前代码，文件路径都是重构后的（`src/` 分层）。

---

## 〇、5 分钟速览（先记这 6 句话）

1. 整个系统是 **两个独立的进程**：`thttpd.out`（web 服务器，多线程）和 `collector.out`（采集进程，守护模式）。
   它们**不直接通信**，全靠两种 IPC：**共享内存**（采集→web，传实时值）和**消息队列**（web→采集，传指令）。
2. 浏览器只跟 web 进程打交道；web 进程只跟共享内存/消息队列/SQLite 打交道；**只有采集进程碰 Modbus 设备**。
3. 一次 HTTP 请求的路径是：`accept` → 开线程 → `handler_msg()` 解析 → **三分流**（静态文件 / 业务 / 404）→ 关连接。
4. `/api/xxx` 这一层（REST）**自己发完整的 HTTP 响应**（包括 401 状态码），所以 thttpd 必须先发的 "200 OK" 要跳过它。
5. 采集进程是"父进程监管 + 子进程干活"：子进程死了父进程 3 秒后重新拉起（最多 20 次）。
6. 数据库 `sensor.db` 里 4 张表：`users` / `sensor_data` / `logs` / `sessions`；密码存的是 `盐$SHA256(盐+密码)`。

---

## 一、系统全景

```
        ┌──────────────────────────┐
        │  浏览器                   │
        │  /app/index.html  登录注册 │
        │  /app/dashboard.html 看板  │  ECharts 画曲线 + 每 2 秒轮询 /api/realtime
        └────────────┬─────────────┘
                     │ HTTP/1.0 短连接（每请求一个连接）
                     ▼
   ╔═══════════════════════════════════════════════════════════════════╗
   ║  web 进程：thttpd.out        (src/main.c + src/http/*)             ║
   ║                                                                    ║
   ║  main()  ─accept循环─► pthread_create ─► handler_msg(sock)         ║
   ║                                              │                     ║
   ║                        ┌─────────────────────┼──────────────────┐  ║
   ║                        │                     │                  │  ║
   ║                  静态文件存在?            /api/ 开头?        其余 → 404
   ║                        │                     │                     ║
   ║                   echo_www()            api_dispatch()             ║
   ║                   （sendfile 零拷贝）    （api_rest.c）            ║
   ║                                              │                     ║
   ║                                    ┌─────────┼─────────┐            ║
   ║                                    │         │         │            ║
   ║                              读共享内存   投消息队列   读写 SQLite    ║
   ╚════════════════════════════════════┼─────────┼─────────┼═══════════╝
                                        │         │         │
                    ┌───────────────────▼──┐  ┌───▼─────────▼──────────┐
                    │ 共享内存 shm          │  │ 消息队列 mq             │
                    │ key=0x123456          │  │ /modbus_sensor_mq       │
                    │ shm_sensor_data_t     │  │ mq_msg_t{cmd,param}     │
                    │ + 进程间 pthread 读写锁│  │ 最大 10 条消息          │
                    └───────────────────▲──┘  └───▲─────────────────────┘
                                        │写        │收（10ms 超时轮询）
   ╔════════════════════════════════════┼──────────┼═══════════════════════╗
   ║ 采集进程：collector.out                    (src/collector/*.c)          ║
   ║                                                                        ║
   ║  父进程 supervise_loop()  ──fork──►  子进程 worker_main()               ║
   ║    · 监管、崩溃后 3 秒重新拉起            · 建共享内存、开消息队列        ║
   ║    · 写 run/collector.pid                · 连 Modbus 设备、周期读寄存器  ║
   ║    · 最多重启 20 次                      · 写共享内存 + 写 SQLite       ║
   ║                                          · 超阈值告警、断线重连、写 syslog║
   ╚══════════════════════════════════════════════╤═════════════════════════╝
                                                  │ Modbus TCP (5020) / RTU
                                                  ▼
                                     设备 / 模拟器 tools/modbus_slave.py
```

**为什么是两个进程？** 采集要长时间稳定运行，web 要频繁重启调试；分开以后
"改 web 代码重启 web" 不影响采集，"设备断了重连" 也不会把 HTTP 服务带崩。这也是工业场景的常规做法（采集/组态分离）。

---

## 二、目录与文件职责

| 文件 | 行数 | 干什么 | 关键函数 |
|---|---|---|---|
| `src/main.c` | 50 | accept 循环，每个连接开一个线程 | `main()`, `msg_request()` |
| `src/http/thttpd.c` | 497 | HTTP 解析、静态文件、错误页、分流 | `init_server()` `handler_msg()` `handle_request()` `echo_www()` `get_line()` `read_headers()` |
| `src/http/custom_handle.c` | 231 | 业务分发：老接口 + 教学接口 + 转 REST | `parse_and_process()` `handle_login()` `handle_add()` `handle_realtime_raw()` |
| `src/http/api_rest.c` | 625 | REST 8 个接口 + token 会话 + JSON 封装 | `api_dispatch()` `api_register()` `api_realtime()` `api_command()` |
| `src/ipc/shm.c/.h` | 126/49 | 共享内存 + 进程间读写锁 + 版本校验 | `shm_sensor_create()` `shm_sensor_attach()` `shm_rdlock()` |
| `src/ipc/mq.c/.h` | 72/31 | POSIX 消息队列（web 发指令给采集） | `mq_send_cmd()` `mq_recv_msg()` |
| `src/db/db.c/.h` | 603/48 | SQLite 封装 + 纯 C SHA-256 + 会话 + 日志 | `db_open()` `db_user_login()` `db_query_history_hours()` `db_session_check()` |
| `src/collector/modbus_collector.c` | 838 | 采集进程：配置/守护/采集循环/状态 | `worker_main()` `supervise_loop()` `daemonize()` `do_status()` |
| `tests/*` | — | 121 项自动化断言 | `run_all_tests.sh` 一键 |
| `tools/modbus_slave.py` | — | 纯标准库 Modbus TCP 从机（没有真设备时用） | — |
| `wwwroot/app/js/*.js` | — | 前端（无构建，原生 JS） | `config.js` `api.js` `auth.js` `dashboard.js` `mock.js` |
| `db/init.sql` | 41 | 建表脚本（**含 DROP**，只在 `make init-db` 用） | — |
| `collector.conf` | 39 | 采集设备配置（只改配置就能换设备） | — |

---

## 三、启动与进程模型

### 3.1 web 进程（多线程、短连接）

```c
// src/main.c:18
int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : 80;   // 不带参数就监听 80（要 sudo）
    int lis_sock = init_server(port);             // src/http/thttpd.c:15
    while (1) {
        int sock = accept(lis_sock, ...);          // 阻塞等连接
        pthread_create(&tid, NULL, msg_request, (void*)(uintptr_t)sock);
    }
}
// src/main.c:6 —— 线程入口
static void *msg_request(void *arg) {
    pthread_detach(pthread_self());   // 线程结束自动回收，主线程不用 join
    handler_msg((int)(uintptr_t)arg); // 干完活线程退出
    return NULL;
}
```

**模型 = "一个连接一个线程"（thread-per-connection）**：
- 优点：写起来直观，一个连接的处理流程是顺序的，不需要状态机。
- 缺点：并发高时线程数量爆炸（每线程默认 8MB 栈），且 `accept` 后立刻建线程有开销。
- 这也是答辩里"下一步能做什么"的标准答案：**改成 epoll 事件驱动 / 线程池**。

`init_server()`（`thttpd.c:15-62`）做了四件事：`socket()` → `SO_REUSEADDR`（避免重启时 TIME_WAIT 占端口）
→ `bind()` → `listen(sock, 5)`。`bind` 失败时它特意把 `errno` 先存下来再打印中文排查提示
（`thttpd.c:39` 的注释：`perror` 可能改写 `errno`）。

### 3.2 采集进程（前台 / 守护两副面孔）

同一个 `collector.out`，靠命令行参数变成不同角色：

| 命令 | 走的分支 | 行为 |
|---|---|---|
| `./collector.out` | `main` → `worker_main()` | 前台跑，日志直接打屏（调试用） |
| `./collector.out --daemon` | `daemonize()` → `supervise_loop()` | 双击 fork 脱离终端，父进程监管子进程 |
| `./collector.out --status` | `do_status()` | 打印 pid + 共享内存里的实时值/极值/点数/周期 |
| `./collector.out --stop` | `do_stop()` | 读 pid 文件，给进程组发 SIGTERM |
| `./collector.out --show-conf` | `cfg_print()` | 打印"配置文件 + 命令行"合并后的生效配置 |

守护化的标准三步（`daemonize()`，`modbus_collector.c:274`）：

```c
pid = fork();  if (pid > 0) exit(0);   // ① 父进程退出 → 子进程被 init(1) 收养，脱离控制终端
setsid();                              // ② 新建会话，成为会话组长，彻底脱离终端
pid = fork();  if (pid > 0) exit(0);   // ③ 再 fork 一次：保证自己不是会话组长（防止意外拿到终端）
umask(0); chdir("/");                  // 清掉文件权限掩码、切到根目录（避免占住某个挂载点）
dup2(log_fd, STDOUT); dup2(log_fd, STDERR); dup2(devnull, STDIN);  // 日志重定向到 logs/collector.log
write_pid_file(getpid());              // 写 run/collector.pid，--stop 靠它找到进程
```

> 注意：`chdir("/")` 之后相对路径全失效，所以 `prepare_paths()`（`modbus_collector.c:645`）
> 在启动时就把 DB/日志/pid **算成绝对路径**。这是 daemon 化的经典坑。

监管逻辑（`supervise_loop()`，`modbus_collector.c:576`）：fork 出 worker → `waitpid` 等它 →
若 worker **正常退出(exit 0)** 则父进程也退出；若是**被信号杀死或非 0 退出**，记 ERROR、等 3 秒再拉起，
累计 20 次（`MAX_RESTARTS`）就放弃。收到 `--stop`（SIGTERM）时给子进程也发 SIGTERM 并退出。

---

## 四、一次 HTTP 请求的完整旅程

以浏览器打开 `http://<IP>:8080/app/index.html` 和 `GET /api/realtime` 为例，逐段拆。

### 4.1 解析请求行（`handler_msg`，`thttpd.c:333`）

```
① recv(MSG_PEEK) 偷看整段原始报文并打印（#if 1 开关，调试用，不是 bug）
② get_line(sock, buf) 读第一行： "GET /app/index.html HTTP/1.1"
③ 手写切分（不用 strtok，教学版逐字符处理）：
     - 第 1 段：method       ← 空格前的内容
     - 第 2 段：url / query  ← '?' 之前的存 url，'?' 之后的存 query_string
④ 方法合法性：不是 GET/POST → clear_header() → echo_error(405)
⑤ need_handle 标记：POST=1；GET 带 query=1；url 以 "/api/" 开头=1
```

`get_line()`（`thttpd.c:72`）是本文件最"教学"的函数：**一次只 recv 1 个字节**，
读到 `\r` 时用 `MSG_PEEK` 偷看下一个字节是不是 `\n` 来决定要不要把两个字符一起吃掉。
好处是能精确处理 `\r\n` / 只有 `\r` / 只有 `\n` 三种换行；坏处是每个字节一次系统调用，性能很差
（注释里写明了"仅适合教学演示"）。

### 4.2 三个安全/健壮性处理（都是踩坑后补的）

```c
// ① 目录穿越防护（thttpd.c:438）：url 里出现 ".." 直接 403
if (strstr(url, "..") != NULL) { clear_header(sock); echo_error(sock,403); goto end; }
//   不加这条的话：GET /../sensor.db 会把整个数据库文件下载走，GET /../src/db/db.c 能下载源码

// ② 目录请求默认页（thttpd.c:453）
char path[SIZE]; sprintf(path, "wwwroot%s", url);
if (path[strlen(path)-1] == '/') strcat(path, "app/index.html");
//   所以访问 http://IP:8080/   →  wwwroot/app/index.html（ModbusGateway 登录页）
//   想看课程原始教学首页要显式访问  /index.html

// ③ 分流顺序（thttpd.c:462-492）—— 顺序不能乱
int is_static_file = (stat(path,&st)==0 && S_ISREG(st.st_mode));
if (is_static_file && GET)   →  echo_www()        // 磁盘上真有这个文件，优先发文件（带 ?参数也照发）
else if (need_handle)        →  handle_request()   // 剩下该走业务的（POST / /api/xxx）
else                         →  echo_error(404)
```

> 为什么"静态优先"？早期版本是 `stat` 失败才走业务，结果 `POST /login` 因为磁盘上没有 `wwwroot/login.html`…
> 哦不，`login.html` 是存在的，所以 POST 被当成静态请求 → 返回页面而不是登录结果；
> `GET /index.html?v=1` 也被当成业务请求返回了一串 JSON。改成"静态优先"两个问题一起解决。

### 4.3 `handle_request`：请求体 + 请求头（`thttpd.c:275`）

```c
int content_len = -1;  char auth[128] = {0};  char req_buf[4096] = {0};
read_headers(sock, &content_len, auth, sizeof(auth));   // 一次读完请求头，顺手取出两样东西
if (POST) {
    if (content_len < 0) content_len = 0;
    if (content_len > sizeof(req_buf)-1) content_len = sizeof(req_buf)-1;   // ← 长度夹紧，防栈溢出
    recv(sock, req_buf, content_len, 0);                                    // 读请求体
}
if (strncmp(url, "/api/", 5) != 0)      // ← 关键：/api/ 的响应由业务层自己发
    send(sock, "HTTP/1.1 200 OK\r\n\r\n", ...);
parse_and_process(sock, url, query_string, req_buf, auth);   // 交给业务层
```

两个设计点值得记住：

1. **`Content-Length` 是客户端给的任意数字**，直接拿去 `recv` 会踩爆栈缓冲区（教学版原代码的 bug），
   所以先夹到 `sizeof(req_buf)-1`。
2. **状态码的归属问题**：HTTP 响应第一行（状态行）只能发一次。如果 thttpd 抢先把 `200 OK` 发出去，
   REST 层后面想发 `401` 就晚了（客户端会看到"200 里包着 401"这种怪东西）。
   所以对 `/api/` 开头的请求，thttpd **不发**状态行，完全交给 `api_rest.c` 的
   `send_json_status()` 自己发（`api_rest.c:112`）。

### 4.4 业务分发（`parse_and_process`，`custom_handle.c:186`）

```
url 以 "/api/" 开头      → api_dispatch()          （REST 层，自己发完整响应）
url 以 "/api"  开头      → 老接口 cmd=realtime/interval/history（裸 JSON，realtime.html 在用）
input 里有 username=/password=  → handle_login()    （老页面 wwwroot/login.html）
input 里有 data1=/data2=        → handle_add()      （求和练习）
其余                     → {"message":"Hello, client!"}
```

### 4.5 REST 层（`api_dispatch`，`api_rest.c:566`）

```c
cJSON *body = (input[0]=='{') ? cJSON_Parse(input) : NULL;   // JSON 请求体

if (url == "/api/register")   api_register(sock, body);
else if (url == "/api/login") api_login(sock, body);
else if (url == "/api/logout")api_logout(sock, auth);
else if (require_login(sock, auth, user, sizeof(user)) != 0) { /* 已回 401 */ }   // ← 后面全部要登录
else if (url == "/api/realtime") api_realtime(sock);
else if (url == "/api/history")  api_history(sock, query_string);
else if (url == "/api/command")  api_command(sock, body);
else if (url == "/api/status")   api_status(sock);
else if (url == "/api/logs")     api_logs(sock, query_string);
else send_wrapped(sock, 404, 404, "接口不存在", NULL);
```

**鉴权是一次性的**：`require_login` 放在中间，上面的注册/登录/退出不需要 token，下面的全需要。
`require_login` 失败时直接调用 `send_wrapped(sock, 401, 401, ...)` 并返回 -1，
后面的 `else if` 链就短路了 —— 这是"一个 if-else 链同时做鉴权和路由"的写法，简单但不显式，
读的时候要注意 `require_login` 有副作用（会发响应）。

统一响应封装：`send_wrapped(sock, http状态码, 业务code, message, data_json)`（`api_rest.c:130`），
内部用 cJSON 拼 `{"code":..,"message":..,"data":..}`，再调 `send_json_status()` 发出去。

---

## 五、采集进程的旅程（`worker_main`，`modbus_collector.c:316`）

### 5.1 启动准备（一次性）

```
① shm_sensor_create(&shm)     建共享内存（web 那边只 attach，不建）
② mq_modbus_open(1)           打开/创建消息队列
③ db_open(g_db_path) + db_init_runtime()   打开 SQLite 并补建表（IF NOT EXISTS，不动老数据）
④ shm 写锁 → 填 start_time / slave_id / period / threshold / 数据来源标记 → 解锁
⑤ modbus_new_tcp() 或 modbus_new_rtu() → modbus_set_slave() → modbus_connect()
     连不上就写 ERROR 日志 + 打印排查建议，然后清资源退出（父进程看到非 0 退出会重新拉起，
     所以设备没插好时你会看到日志里反复"重连"）
```

### 5.2 主循环（每轮干三件事）

```c
while (g_running) {                       // modbus_collector.c:416
    /* 1) 收 web 下发的指令：最多阻塞 10ms，没指令就往下走 */
    if (mq_recv_msg(mq_fd, &cmd_msg, 10) == 0) {
        CMD_SET_INTERVAL  → g_cfg.interval_ms = param;  shm->period_ms = param;
        CMD_SET_THRESHOLD → g_cfg.threshold = param/100.0f;  shm->threshold = ...;
        CMD_QUERY         → 地址=param>>16, 个数=param&0xFFFF
                            modbus_read_registers() → 结果写进 shm->last_regs[]
        /* CMD_RESTART 这里没有分支：网页上点"重启"只记日志，不真重启（见 docs/API.md） */
    }

    /* 2) 读设备 */
    int rc = modbus_read_registers(ctx, g_cfg.reg_base, g_cfg.reg_count, reg_buf);
    if (rc == -1) {                          // 读失败
        shm->device_status = 0;              //   标记离线
        30 秒最多记一条日志;                  //   限流，避免日志被刷爆
        if (++modbus_err_count >= 5) {        //   连续失败 5 次
            modbus_close(ctx); sleep(1);      //   断开
            modbus_connect(ctx) 重连;          //   成功了就清零计数，下一轮自动恢复采集
        }
        continue;
    }

    /* 3) 换算 + 判定 + 落两处 */
    float temp = reg_buf[iv] / g_cfg.scale_temp;      // 寄存器原始值 ÷ 量纲
    float humi = reg_buf[ih] / g_cfg.scale_humi;
    int status = (temp > g_cfg.threshold) ? 2 : 1;    // 2=故障(超阈值)  1=在线
    if (status == 2 && !was_over_threshold) log_msg("WARN", ...);   // 边沿触发，只报一次
    写共享内存（加写锁）：temperature/humidity/status/timestamp/sample_count++/temp_max/temp_min/last_regs
    db_insert_sensor_record(g_db, temp, humi, status);             // 历史入库
    usleep(g_cfg.interval_ms * 1000);                              // 睡一个采集周期
}
```

**"边沿触发"和"限流"是这一段最值得讲的两个细节**：温度一直在阈值上面时，
不会每个周期都刷一条 WARN（`was_over_threshold` 保证只在"刚超过去"的那一刻报一次）；
Modbus 读失败也只在 30 秒内报一条。工业项目的日志必须这么控制，否则真正的问题会被淹掉。

### 5.3 退出清理（`modbus_collector.c:561`）

`SIGINT/SIGTERM` → `g_running = 0` → 跳出循环 → `modbus_close` / `db_close` /
`mq_close` + `mq_unlink` / `shm_detach` + `shm_destroy`。
**顺序很重要**：先停数据来源，再关通道，最后销毁共享内存（否则 web 可能读到半截状态）。

---

## 六、进程间通信（IPC）细节

### 6.1 共享内存：`shm_sensor_data_t`（`src/ipc/shm.h:13`）

```c
typedef struct {
    int magic;                     // 必须 == SHM_MAGIC(0x53454E53 "SENS")，挂接时校验
    pthread_rwlock_t rwlock;       // 进程间读写锁（PTHREAD_PROCESS_SHARED）
    float temperature, humidity;
    float temp_max, temp_min;      // 本次运行的极值（KP I 卡片上的最高/最低）
    unsigned int sample_count;     // 本次运行已采集点数
    time_t timestamp, start_time;  // 最近采集时刻 / 启动时刻（算 uptime、判断数据新鲜度）
    int device_status;             // 0 离线  1 在线  2 故障(超阈值)
    int slave_id, period_ms;  float threshold;
    unsigned short last_regs[8];   // 最近一次读到的寄存器原始值（readRegister 接口回值用）
    int last_regs_addr, last_regs_count;
    char source_name[48];          // "modbus-simulator" / "车间1号温湿度计"
    int  source_simulated;         // 1=模拟数据 0=真实设备
    char device_target[64];        // "tcp 127.0.0.1:5020 从机1"
} shm_sensor_data_t;
```

三处关键设计：

| 设计 | 代码 | 为什么 |
|---|---|---|
| **magic 版本校验** | `shm.c:62` 最后才写 magic；`shm.c:80` 挂接时对不上就失败 | 结构体改过之后，旧段的大小/布局不一样，直接读会读到垃圾。写作端"最后写 magic"= 写完才算有效 |
| **create 自动清理旧段** | `shm.c:28` 遇 `EINVAL` 就 `shmctl(IPC_RMID)` 删掉重建 | 改了结构体不用手工 `ipcrm`，采集进程能直接起来 |
| **attach 不带 `IPC_CREAT`** | `shm.c:74` | web 进程不能凭空创建共享内存，否则"采集没跑"会被误判成"有数据" |

读写锁用 `pthread_rwlock_timedrdlock / timedwrlock` **带超时**（web 读 300ms、采集写 1000ms）：
拿不到锁就返回错误，绝不永久阻塞。这是"多进程共享内存"最容易出事的地方 ——
只要有一个进程在持锁时崩了，不带超时的话另一个进程就永久挂死。

### 6.2 消息队列（`src/ipc/mq.c`）

```c
mq_open("/modbus_sensor_mq", O_RDWR|O_CREAT, 0666, &attr);   // attr: 最多 10 条, 每条 sizeof(mq_msg_t)
```

| 指令 | 值 | param 的含义 | 编码方式 |
|---|---|---|---|
| `CMD_QUERY` | 1 | 起始寄存器地址 + 个数 | `(addr << 16) | count` 打包进一个 int，采集侧 `>>16` / `&0xFFFF` 拆开 |
| `CMD_SET_INTERVAL` | 2 | 毫秒 | 直接是数值 |
| `CMD_SET_THRESHOLD` | 3 | 阈值 ×100 | `32.5℃ → 3250`，因为 param 是 int 不能存小数 |
| `CMD_RESTART` | 4 | 无 | 采集侧暂无分支（只记录） |

`mq_recv_msg` 用 `mq_timedreceive`，注意 `tv_nsec` 必须 < 1e9 否则直接返回 `EINVAL`（`mq.c:50` 做了进位归一化）——
这是从 `mq_timedreceive` 那里踩过的坑。

---

## 七、数据库层（`src/db/db.c`）

### 7.1 四张表

| 表 | 谁写 | 谁读 | 关键字段 |
|---|---|---|---|
| `sensor_data` | 采集进程每周期插一条 | `/api/history`、`/api/realtime` 的 count | temperature, humidity, device_status, timestamp |
| `users` | `/api/register` | `/api/login` | username(UNIQUE), password_hash(`盐$SHA256(盐+密码)`) |
| `sessions` | 登录时写入 | 每次带 token 的请求 | token(PK), username, role, **expire(unix 时间戳)** |
| `logs` | 采集进程 + web（db_log） | `/api/logs` | time, level(DEBUG/INFO/WARN/ERROR), module, message |

### 7.2 建表的两种路径（别搞混）

| 函数 | 用在哪 | 行为 |
|---|---|---|
| `db_run_init_sql()`（`db.c:183`） | `make init-db`、`tests/db_selftest.c` | 执行 `db/init.sql`，**里面是 `DROP TABLE` + `CREATE`** → 清空历史！ |
| `db_init_runtime()`（`db.c:217`） | 两个进程启动时各自调一次 | 全部 `CREATE TABLE IF NOT EXISTS`，**不动已有数据** |

> 这是刻意的：答辩/演示前想清库跑 `make init-db`；平时启动只做"缺表补建"。
> 曾经的 bug 是让 collector 每次启动都执行 `init.sql`，结果历史数据每次被清空。

### 7.3 一条容易被问到的 SQL（`db_query_history_hours`，`db.c:397`）

```sql
SELECT time, temperature, humidity FROM (
    SELECT strftime('%Y-%m-%d %H:%M:%S', timestamp, 'localtime') AS time,
           temperature, humidity, id
    FROM sensor_data WHERE timestamp >= datetime('now', ?)     -- ? = "-24 hours"
    ORDER BY id DESC LIMIT ?                                   -- 先取最新 N 条
) ORDER BY id ASC;                                             -- 再翻回时间升序（图表从左到右）
```

要点：**子查询里 DESC + LIMIT 取"最近 N 条"，外层 ASC 排回正序**。
如果直接 `ORDER BY time ASC LIMIT N`，取到的是"最早 N 条"，图表就不动了。

另外 `strftime(..., 'localtime')` 是在**查询时**把库里存的 UTC 转成当地时间 ——
注意 `sensor_data.timestamp` 是用 `CURRENT_TIMESTAMP` 存的 **UTC**，而 `logs.time` 存的是
`datetime('now','localtime')` 本地时间。两张表时间口径不一致，是已知的小瑕疵（见"已知限制"）。

### 7.4 会话为什么放数据库

`session_create/check/remove`（`api_rest.c:80-101`）只是薄封装，真正在 `db_session_*`（`db.c:468-534`）。
会话放库里的好处：
1. **web 进程重启后 token 依然有效**（放进程内存里的话，`make down/up` 之后前端每个请求都 401，
   看板直接空白 —— 这是踩过的坑，`tests/test_session_persist.sh` 就是防它回归的）；
2. 同时开 80 和 8080 两个实例也能互相认账；
3. 过期清理只要一句 SQL（`db_session_cleanup`，登录时顺手调一次）。

---

## 八、用户、会话与安全

### 8.1 注册/登录的密码处理（`db.c:116-165`、`265-328`）

```
注册：make_salt() 从 /dev/urandom 取随机字节 → 转十六进制当盐
      db_make_hash(password, salt) = SHA-256(盐 + 密码) 的十六进制
      入库格式：  "盐$哈希"        ← 盐和哈希一起存，登录时再取出来复算比对
登录：SELECT password_hash FROM users WHERE username=?
      按 '$' 拆出盐 → 用同一套算法算一遍 → strcmp 比对成功才发 token
```

**为什么这样是对的**：库里没有明文；同样的密码因为盐不同，哈希也不同（防彩虹表）；
验证时不需要解密（哈希不可逆）。

`db.c` 里的 SHA-256 是**自己用纯 C 实现的**（`sha256_transform/init/update/final`，`db.c:42-115`），
不依赖 OpenSSL。自测里用官方测试向量校验：`sha256("abc")` 必须等于
`ba7816bf...15ad`、`sha256("")` 必须等于 `e3b0c442...b855`（`tests/db_selftest.c`）。

### 8.2 token 与鉴权链路

```
登录成功 → gen_token()（/dev/urandom 取 16 字节 → 32 位十六进制，`api_rest.c:57`）
        → 写 sessions 表，expire = now + 7200 秒
前端把 token 存 sessionStorage，之后每个请求带  Authorization: Bearer <token>
后端 bearer_token() 切掉 "Bearer " 前缀 → session_check() 查库并检查 expire > now
   查不到/过期 → require_login() 回 HTTP 401 → 前端 api.js 清登录态并跳回登录页
```

### 8.3 其它安全处理（都在代码注释里标了"【必须判】"）

| 风险 | 处理位置 | 做法 |
|---|---|---|
| 目录穿越（下载数据库/源码） | `thttpd.c:438` | url 含 `..` → 403 |
| 请求体超长踩爆栈 | `thttpd.c:297` | `Content-Length` 夹到 4095 |
| URL 超长越界写 1 字节 | `thttpd.c:80` | `while(i < SIZE-1)`，留位置给结尾 `\0` |
| 405/403/404 响应被 RST 冲掉 | `thttpd.c:384` | 先 `clear_header()` 把请求剩余数据读完再回响应 |
| 404 页面文件被删导致崩溃 | `thttpd.c:140` | `stat` + `open` 双判断，失败发纯文本兜底 |
| 非法异常请求导致进程崩 | `tests/tests_web.py` | 全部异常请求后断言"服务进程仍存活" |
| SQL 注入 | 全库 | 一律 `sqlite3_prepare_v2` + `sqlite3_bind_*` 参数化，不拼字符串 |

> 关于"先读完请求再回错误"这条：TCP 上服务端 `close()` 时如果接收缓冲区还有未读数据，
> 内核会发 **RST**，把已经发出去的响应一起冲掉，浏览器只看到"连接被重置"。
> 所以 405/403 这些提前结束的分支必须先 `clear_header()`。

---

## 九、前端（`wwwroot/app/`，原生 JS 无构建）

| 文件 | 职责 |
|---|---|
| `index.html` + `auth.js` | 登录/注册页（两个 tab 切换），成功后 `API.Session.save()` 存 token 跳到看板 |
| `js/config.js` | 唯一配置点：`MOCK`（是否用本地假数据）、`API_BASE:'/api'`、`POLL_INTERVAL:2000`、`REALTIME_POINTS:150`、阈值 |
| `js/api.js` | 统一出口 `window.API.*`：`request()` 自动加 `Authorization`、**遇 401 自动清登录态并回登录页**；`MOCK=true` 时走 `window.Mock` |
| `js/dashboard.js` | 看板逻辑：ECharts 实时曲线（滚动窗口 150 点）+ 历史曲线、KPI 卡片、状态灯、**模拟/真实数据横幅**、日志表、指令下发按钮；`setInterval` 按 2 秒轮询 |
| `js/mock.js` | 纯前端假数据（后端没好时演示用），`MOCK=false` 时完全不参与 |
| `css/style.css` | 暗色主题 + 卡片/看板样式 |

数据流（看板）：

```
setInterval(2000)
   ├─ API.realtime()  → /api/realtime → 读共享内存 → 更新 KPI + 实时曲线（push 一个点，多于150个就 shift）
   ├─ API.status()    → /api/status   → 进程在线状态灯 + 数据来源（模拟/真实）横幅
   └─ API.logs()      → /api/logs     → 日志表格
用户点按钮 → API.command('setPeriod'|'setThreshold'|'readRegister') → 消息队列 → 采集进程
```

作业答辩时可以强调：**前端没打包工具、没框架，纯 ES5 + fetch + ECharts**，
刷新页面即部署，`config.js` 一行 `MOCK` 就能切换"独立演示"和"连真后端"两种模式。

---

## 十、关键设计取舍（答辩加分项）

| 选择 | 好处 | 代价 / 替代方案 |
|---|---|---|
| 手写 HTTP 服务器（不用 nginx/libevent） | 能讲清楚 HTTP 协议每一行怎么解析 | 性能、健壮性远不如成熟服务器；替代：epoll + 状态机 |
| 一连接一线程 | 代码直观，处理流程顺序化 | 并发受限；替代：线程池 / epoll |
| 两个进程 + 共享内存 + 消息队列 | 采集与 web 解耦、崩溃互不影响；**能完整展示 IPC 技术栈**（这是课设要求的重点） | 需要处理锁、超时、结构体版本；比"单进程多线程"复杂 |
| 共享内存而不是每次都查库 | 实时值读取是微秒级，不碰磁盘 | 需要锁和 magic 校验 |
| 消息队列而不是信号/管道 | 有消息边界、能排队、能带参数、带超时可阻塞等待 | 需要 `-lrt`；队列有长度上限（10 条） |
| 会话存 SQLite | 重启不丢登录 | 每次请求多一次查库（教学场景无压力） |
| 纯 C 实现 SHA-256 | 不引入 OpenSSL 依赖，能讲清哈希原理 | 代码长、要自测（官方测试向量） |
| 配置外置 `collector.conf` | 换设备不改代码不重编译 | 要处理行内注释、命令行覆盖优先级 |
| 动态分配历史缓冲 | 数据涨到上千点不会被截断 | 要注意释放（free）；固定 8192 缓冲踩过坑 |

---

## 十一、代码里的"必须判"（踩坑清单，注释里都留了记录）

1. `perror()` 可能改写 `errno` → 判断前先 `int saved = errno;`（`thttpd.c:39`）
2. `get_line` 的循环上界必须 `SIZE-1`，否则结尾写 `\0` 越界 1 字节（`thttpd.c:80`）
3. `Content-Length` 必须夹紧（`thttpd.c:297`）
4. 405/403/路径穿越 之前必须 `clear_header()`，否则 RST 冲掉响应（`thttpd.c:384`）
5. `show_404` 里 `stat`/`open` 都要判，否则脏 `st_size` + 非法 fd 传给 `sendfile` 会崩（`thttpd.c:140`）
6. `echo_error` 里**不能**调 `clear_header` —— `echo_www` 出错进来时请求头已经读完，再读会阻塞挂死线程（`thttpd.c:189`）
7. `shmctl` 判断要用 `shmid >= 0`，`shmid` 从 0 开始编号，`>0` 会漏掉 id=0 的段（`shm.c:98`）
8. `tv_nsec` 必须归一化到 < 1e9，否则 `pthread_rwlock_timed*` / `mq_timedreceive` 直接返回 `EINVAL`（`shm.c:16`、`mq.c:50`）
9. 不定长列表**绝对不能用固定栈缓冲**装 JSON（历史 150+ 行会被截断，前端静默拿到空列表）→ 改 `malloc`（`api_rest.c:325` 注释）
10. 配置解析要剥行内注释，否则 `host = 127.0.0.1  # 注释` 整串被当 IP，采起来就死
11. daemon 化后 `chdir("/")`，所有路径必须在启动时算成绝对路径（`modbus_collector.c:645`）
12. 前台服务会占住终端（`./thttpd.out` 不回提示符是正常的）；不带端口参数=监听 80=要 sudo
13. Ctrl+Z 挂起的进程端口还占着且 SIGTERM 对它无效，必须先 `kill -CONT` 或 `kill -9`

---

## 十二、答辩常见追问（Q&A）

**Q1 为什么用共享内存 + 消息队列，不用一套？**
两者语义不同：实时值要"读最新的、越快越好、不关心历史"→ 共享内存（微秒级、无系统调用开销）；
指令要"可靠送到、有边界、能排队、能带参数和超时"→ 消息队列。
用共享内存传指令就得自己实现队列+通知机制；用消息队列传实时值则要轮询消费，反而更慢。

**Q2 共享内存里为什么要放 pthread 读写锁？跨进程的锁是怎么工作的？**
`pthread_rwlockattr_setpshared(PTHREAD_PROCESS_SHARED)` 之后，锁对象可以放在共享内存里被多个进程使用。
采集进程持写锁只做一件事：把最新值搬进去；web 读时持读锁把值拷到自己的栈变量，然后立刻解锁 ——
**"锁内只拷数据，不在锁内做 IO/拼 JSON"**，这是缩小临界区的基本功。
另外两边都用 `timed*` 版本（web 300ms、采集 1000ms），拿不到锁就返回错误，绝不永久阻塞。

**Q3 消息队列满了怎么办？丢了指令用户知道吗？**
`mq_maxmsg = 10`。队列满时 `mq_send` 返回 -1（`EAGAIN`），
`mq_send_cmd()` 的返回值会被 `api_command()` 放进响应里的 `send_rc` 字段，前端能看出来；
同时 web 侧会写一条 `logs` 日志。目前**没有做重试**，属于可改进点。

**Q4 采集进程崩了怎么办？**
两层自愈：进程级 —— 父进程 `waitpid` 发现子进程异常退出就 3 秒后重新拉起（最多 20 次）；
连接级 —— Modbus 连续读失败 5 次就 `modbus_close` + `modbus_connect` 重连。
再往外还有 `tools/start_all.sh` 的一键拉起和 systemd/手工 `--daemon`。

**Q5 怎么保证历史数据量大了前端不卡？**
两道闸：后端 `HISTORY_MAX_POINTS = 1000`（单次最多 1000 点，256KB 动态缓冲，超了返回最新的）；
前端实时曲线只保留 150 个点（`REALTIME_POINTS`），旧的 `shift()` 掉。
数据库层面 `WHERE timestamp >= datetime('now','-N hours')` 走时间过滤，先 DESC 取最近 N 条。

**Q6 密码怎么存的？能反解出来吗？**
`盐$SHA256(盐+密码)`，盐来自 `/dev/urandom`，SHA-256 是单向下压函数，不能反解；
同样的密码因为盐不同哈希也不同。库里绝对没有明文（自测里专门断言了"库里没有明文密码"和"salt$hash 格式"）。

**Q7 token 为什么放数据库不放内存？**
放内存的话 web 进程一重启，所有人的登录态全没了（前端每个请求 401 → 看板空白）。
放库里重启不丢，还能让 80/8080 两个实例共享会话。`tests/test_session_persist.sh` 专门测这条。

**Q8 一次请求的开销在哪？怎么优化？**
`get_line` 逐字节 `recv`（每字节一次系统调用）是最大头；其次是每连接新建线程；
再就是 `printf` 调试输出。优化方向：改成按块读 + 缓冲区分行解析、线程池或 epoll、
调试打印加开关/分级。

**Q9 为什么不用线程池/epoll 直接做？**
课设的重点是"把 HTTP 协议、IPC、SQLite、Modbus 每条链路都亲手写通"，
thread-per-connection 让每条链路是顺序代码，最好讲清楚；
epoll/reactor 放在扩展阶段（README 的"已知限制"里也写了）。

**Q10 如果设备换成 4~20mA 模拟量采集器怎么办？**
改 `collector.conf` 的 `transport/host/port/slave/reg_*/scale_*` 就行，
量纲不同就用 `scale_temp` 除 —— 代码不用重编译。这也是把配置外置的原因。
`./collector.out --show-conf` 能看到合并后的生效配置，方便核对。

---

## 十三、改代码地图（想动手改的话看这里）

| 想做的事 | 要改的文件（按顺序看） |
|---|---|
| 加一个 REST 接口 | `src/http/api_rest.c`（写 `api_xxx()` + 在 `api_dispatch` 加分支）→ `docs/API.md` 补契约 → `tests/tests_api_rest.py` 加断言 |
| 改共享内存里的字段 | `src/ipc/shm.h`（结构体）→ 采集侧写入 `modbus_collector.c` → 读取侧 `api_rest.c`（realtime/status）→ **不用手工清共享内存**（`shm_sensor_create` 会因 `EINVAL` 自动重建） |
| 加一种消息队列指令 | `src/ipc/mq.h`（加 `CMD_xxx` 和 param 约定）→ 采集侧 `worker_main` 的收指令分支 → web 侧 `api_command` → `docs/API.md` |
| 换采集设备 | 只改 `collector.conf`（或命令行 `--host/--port/--slave/--real`），不重编译 |
| 加数据库表/字段 | `db/init.sql`（新环境用）+ `db.c` 的 `db_init_runtime()`（运行期补建，两处都要加）→ `db.h` 声明接口 |
| 改 HTTP 解析/分流规则 | `src/http/thttpd.c` 的 `handler_msg()`（解析+分流）、`handle_request()`（请求体/状态码归属）→ 改完必跑 `tests/tests_web.py`（27 项覆盖静态/405/穿越/超长 URL 等边界） |
| 改前端页面 | `wwwroot/app/`（无构建，改完刷新浏览器即可；`js/config.js` 是唯一配置点） |
| 加测试 | 放 `tests/`，并在 `tests/run_all_tests.sh` 里加一段；跑 `make test` 应保持全绿 |

**改动验收铁律**：`make clean && make`（必须 0 warning）→ `make test`（121 项全绿）→
随手 `md5sum` 存档关键文件（防止"改了没生效"其实是编辑器打开了别的目录/旧缓冲区覆盖回去）。
