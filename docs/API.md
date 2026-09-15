# 接口契约（ModbusGateway）

工程里其实有三套接口，按"新 → 老 → 教学"三个层次并存，改代码时别改错地方：

| 层次 | 路径风格 | 实现文件 | 谁在用 |
|---|---|---|---|
| RESTful（新） | `/api/xxx`，JSON 进 JSON 出，token 鉴权 | `src/http/api_rest.c` | ModbusGateway 前端（`wwwroot/app/`） |
| 老接口 | `/api?cmd=xxx`，返回裸 JSON，无鉴权 | `src/http/custom_handle.c` | `wwwroot/realtime.html`、命令行调试 |
| 教学接口 | `POST /login`、`POST /add`，表单 | `src/http/custom_handle.c` | 课程原始页面 `wwwroot/{login,post}.html` |

统一入口是 `parse_and_process()`（`src/http/custom_handle.c`），分发顺序：
`/api/` 前缀 → REST 层；`/api?cmd=` → 老接口；表单字段 → 教学接口；其余返回默认 JSON。

---

## 一、RESTful 接口

统一响应体：

```json
{"code": 0, "message": "ok", "data": { ... }}
```

`code=0` 成功，非 0 见错误码表；HTTP 状态码只有 200 / 401 / 404 三种语义。

除注册、登录外，所有接口都要带请求头：

```
Authorization: Bearer <token>
```

token 缺失或失效 → **HTTP 401** + `{"code":401,...}`。

> token 会话存在数据库 `sessions` 表（TTL 7200 秒），不是进程内存 ——
> 重启 web 服务、换端口、甚至同时开 80/8080 两个实例，旧 token 都还有效。

### 1. POST /api/register

```json
{"username": "admin", "password": "123456"}
```

| code | 含义 |
|---|---|
| 0 | 注册成功，`data{uid,username}` |
| 1002 | 用户名已存在（UNIQUE 约束） |
| 1003 | 用户名或密码为空 |

密码落库是 `salt$sha256(salt+password)`（salt 取自 `/dev/urandom`），**库里没有明文密码**。

### 2. POST /api/login

```json
{"username": "admin", "password": "123456"}
```

成功：`code 0` + `data{username,role,uid,token}`；失败：`1001 用户名或密码错误`。

### 3. POST /api/logout

带 token 调用，token 立即从 `sessions` 表删除，之后用旧 token 访问一律 401。

### 4. GET /api/realtime

读共享内存（加读锁，300ms 超时）拿采集进程写进去的实时值。

```json
{"code":0,"data":{
  "temp":26.2,"humi":67.6,"tempMax":26.2,"tempMin":25.6,"count":128,
  "slaveId":1,"online":true,"collectTime":"2026-09-14 17:47:32",
  "period":2000,"threshold":32.0,
  "source":{"name":"modbus-simulator","simulated":true,"target":"tcp 127.0.0.1:5020 从机1"}
}}
```

采集进程没运行 → `code 2001`（共享内存不存在）。

### 5. GET /api/history?hours=24

```json
{"code":0,"data":{"hours":24,"list":[{"time":"2026-09-14 06:21:43","temp":27.4,"humi":68}]}}
```

时间升序；上限 1000 点 / 256KB（动态分配缓冲，不会像老版本那样被截断）。
`hours<=0` 或缺失时默认 24。

### 6. POST /api/command

```json
{"action":"setPeriod","period":2000}
{"action":"setThreshold","threshold":32}
{"action":"readRegister","addr":0,"count":2}
{"action":"restart"}
```

| action | 参数 | 说明 |
|---|---|---|
| setPeriod | period 100~60000 (ms) | 走消息队列 `CMD_SET_INTERVAL` 下发给采集进程，立即生效 |
| setThreshold | threshold (℃) | 走消息队列 `CMD_SET_THRESHOLD`（内部 ×100 传整数） |
| readRegister | addr, count | 走消息队列 `CMD_QUERY`，采集进程读保持寄存器后回写共享内存，web 侧取回 |
| restart | 无 | **目前只在日志里记一条，不会真的重启**（重启请用 `./collector.out --stop/--daemon`） |

非法 action / 非法参数 → `code 1004`。

### 7. GET /api/logs

```json
{"code":0,"data":{"list":[{"time":"...","level":"INFO","module":"mq","message":"采集周期已修改为 1000 ms"}]}}
```

分级 DEBUG/INFO/WARN/ERROR，倒序，上限 200 条 / 128KB。

### 8. GET /api/status

```json
{"code":0,"data":{
  "collectOnline":true,"webOnline":true,"modbusOnline":true,
  "period":2000,"uptime":4,"lastCollectTs":1789433252,
  "deviceStatus":1,"slaveId":1,
  "source":{"name":"modbus-simulator","simulated":true,"target":"tcp 127.0.0.1:5020 从机1"}
}}
```

采集进程没跑时 `"source":null`（不瞎报"真实设备"）。

### 错误码表

| code | HTTP | 含义 |
|---|---|---|
| 0 | 200 | 成功 |
| 401 | 401 | 未带 token / token 失效 |
| 404 | 404 | 接口不存在 |
| 1001 | 200 | 用户名或密码错误 |
| 1002 | 200 | 用户名已存在 |
| 1003 | 200 | 参数为空 |
| 1004 | 200 | 未知指令 / 参数非法 |
| 2001 | 200 | 采集进程未运行（共享内存不存在） |

---

## 二、老接口（返回裸 JSON，没有 `{code,message,data}` 包装）

供 `wwwroot/realtime.html` 和命令行调试用，与新接口并存、互不影响：

```
GET /api?cmd=realtime                     -> {"temp":..,"humi":..,"status":..,"ts":..,"time":".."}
GET /api?cmd=interval&sec=N               -> {"result":"ok","interval_ms":N*1000,"send_rc":0}    (N: 1~60)
GET /api?cmd=history&limit=N              -> [{"temp":..,"humi":..,"status":..,"time":".."}, ...]  (N: 1~100)
其余 cmd                                   -> {"error":"未知的cmd参数..."}
```

采集进程没跑时 `cmd=realtime` 返回 `{"error":"采集进程未运行(共享内存不存在)"}`。

---

## 三、教学接口（课程原始页面在用）

```
POST /login   表单 username=...&password=...   硬编码 admin/admin，成功返回跳转 JS
POST /add     表单 "data1=1data2=2"            返回两数之和的纯文本
```

> 这一层的登录**没有接数据库用户体系**（还是课程原始的硬编码 admin/admin），
> 新的注册/登录在 `/api/register`、`/api/login`。两套用户体系目前没合并，改的时候别混。

---

## 四、消息队列指令码（`src/ipc/mq.h`）

队列名 `/modbus_sensor_mq`，web 进程投递、采集进程消费。`param` 是 int，要传小数就放大成整数：

| 指令 | 值 | param 含义 |
|---|---|---|
| CMD_QUERY | 1 | 高 16 位 = 寄存器起始地址，低 16 位 = 读取个数 |
| CMD_SET_INTERVAL | 2 | 采集周期 ms |
| CMD_SET_THRESHOLD | 3 | 阈值 ×100（3250 → 32.5℃） |
| CMD_RESTART | 4 | 无参数 |

## 五、共享内存（`src/ipc/shm.h`）

`shm_sensor_data_t`：实时值 + 运行统计（max/min/count/period/threshold/last_regs）+ 数据来源标记
+ `pthread_rwlock_t` 进程间读写锁 + 开头的 `SHM_MAGIC` 版本校验。

- 采集进程持**写锁**更新，web 端持**读锁**拷贝；两边锁都带超时（web 300ms / 采集 1000ms），拿不到就报错，绝不死锁。
- `shm_sensor_create()` 遇到 `EINVAL`（结构体变了）会自动删旧段重建；`shm_sensor_attach()` 先校验 magic。
