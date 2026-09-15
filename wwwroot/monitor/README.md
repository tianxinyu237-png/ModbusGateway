# 高档住宅温湿度采集监控系统 · 前端（monitor）

按《高档住宅温湿度采集监控系统项目需求文档 V2.0》实现的前端页面，原生 HTML + CSS + JS + ECharts，
**无构建步骤**（不需要 npm / webpack / vue-cli），改完直接刷新浏览器就能看到。

---

## 一、怎么跑

### 方式 A：挂到现有 C 后端上（正式用法）

部署时把这个 `monitor/` 整个目录拷进课设工程的静态根 `~/network/lianxi/wwwroot/` 下，
即最终路径是 `wwwroot/monitor/`，和 `thttpd` 的静态根同级：

```bash
cd ~/network/lianxi
make up                       # 一键起 从机模拟器 + 采集守护进程 + web 服务器
```

然后用浏览器打开：

| 页面 | 地址 |
|---|---|
| 登录 / 注册 | `http://<虚拟机IP>:8080/monitor/index.html` |
| 数据看板 | `http://<虚拟机IP>:8080/monitor/dashboard.html`（需先登录） |

> 注意：原来那版看板还在，地址是 `/app/index.html`、`/app/dashboard.html`，两者互不影响。
> 静态根只有 `wwwroot/`，所以页面**必须放在 wwwroot 里面**，放工程根目录会 404。

### 方式 B：纯前端看界面（后端还没起来时）

`js/config.js` 里 `MOCK: 'auto'` 是默认值：先按真后端请求，**一旦发现后端连不上就自动降级成
本地演示数据**，并在页面顶部亮黄条提示。所以直接打开页面就能看到完整界面（含曲线、历史、日志）。

演示账号：`admin` / `123456`

想强制纯演示：`MOCK: true`；想强制只连后端（连不上就报错，不掩盖问题）：`MOCK: false`。

---

## 二、目录结构

```
monitor/
├── index.html        登录 / 注册页（单页双 Tab）
├── dashboard.html    数据看板
├── css/style.css     全部样式（深色工业监控风，两个页面共用）
├── js/
│   ├── config.js     ← 唯一需要按后端情况修改的配置
│   ├── api.js        接口封装层（token、401 跳登录、MOCK 降级调度）
│   ├── mock.js       本地模拟数据（MOCK 模式 / 自动降级时用）
│   ├── auth.js       登录注册页逻辑
│   ├── dashboard.js  看板逻辑（轮询、曲线、指示灯、历史、日志）
│   └── echarts.min.js  ECharts 本地兜底（CDN 挂了自动用这个）
└── README.md         本文件
```

---

## 三、接口对接清单（前端已按这套实现）

统一响应体 `{"code":0,"message":"ok","data":{...}}`；除注册/登录外都要带
请求头 `Authorization: Bearer <token>`；token 失效返回 **HTTP 401**，前端会自动清登录态跳回登录页。

| 方法 | 路径 | 请求 | 前端用到的 data 字段 | 对应需求 |
|---|---|---|---|---|
| POST | `/api/register` | `{username,password}` | — | 3.1 注册 |
| POST | `/api/login` | `{username,password}` | `username,role,uid,token` | 3.1 登录 |
| POST | `/api/logout` | `{}` | — | 3.1 退出 |
| GET | `/api/realtime` | — | `temp,humi,tempMax,tempMin,count,slaveId,online,collectTime,period,threshold,source{name,simulated,target}`，**新增 `led`,`ledAddr`** | 3.2 / 3.3 |
| GET | `/api/history?hours=24` | — | `hours,list[{time,temp,humi}]` | 3.3 历史查询 |
| POST | `/api/command` | `{action,...}` | 见下 | 3.4 设备控制 |
| GET | `/api/logs` | — | `list[{time,level,module,message}]` | 3.5 日志 |
| GET | `/api/status` | — | `collectOnline,webOnline,modbusOnline,period,uptime,source` | 页面顶栏状态灯 |

`/api/command` 的 action：

| action | 参数 | 界面入口 |
|---|---|---|
| `setPeriod` | `period` 100~60000 ms | 设备控制 → 采集周期 → 下发 |
| `setThreshold` | `threshold` ℃ | 设备控制 → 温度告警阈值 → 下发 |
| `readRegister` | `addr`, `count` | 设备控制 → 读取保持寄存器 |
| `restart` | — | （接口保留，界面未放按钮） |
| **`setLed`** | **`on`: 1/0** | **设备控制 → 指示灯开关** ← 后端还没有 |

错误码处理：`1001` 账号密码错误（标红密码框）、`1002` 账号已存在（标红账号框）、
`1003` 参数为空、`1004` 未知指令、`2001` 采集进程未运行（顶部亮红条提示启动 collector）。

---

## 四、后端待补清单（**指示灯控制这一条是必做的**）

需求文档 3.4 和验收标准第 4 条要求"点击页面按钮经消息队列下发指令，采集进程正确置位从机
指示灯寄存器"，但当前后端只有 `CMD_QUERY / CMD_SET_INTERVAL / CMD_SET_THRESHOLD / CMD_RESTART`，
**没有指示灯指令**。前端已经把入口做出来了（开关 + 回读按钮），后端按下面 5 处补上即可联通：

1. `src/ipc/mq.h` — 加一个指令码
   `#define CMD_SET_LED 5`，`param` 传 `1`（置位点亮）/ `0`（复位熄灭）。
2. `src/http/api_rest.c` — `action` 分支加 `setLed`
   取 `on` 字段（0/1）→ `mq_send(CMD_SET_LED, on)` → 返回
   `data{"action":"setLed","led":<on>,"addr":<指示灯寄存器地址>,"send_rc":<rc>}`；
   `on` 不是 0/1 时按 `1004` 返回。
3. `src/collector/modbus_collector.c` — 主循环收指令处加 `case CMD_SET_LED`
   调 `modbus_write_bit(ctx, 指示灯线圈地址, on)`（或 `modbus_write_register`，看从机是线圈还是
   保持寄存器）；成功/失败都要写日志（`db_log_add` 或现有日志接口），满足 3.4"控制操作与结果写入日志记录表"。
4. `src/ipc/shm.h / .c` — 共享内存结构体加 `int led;`（+ `int led_addr;`），采集进程
   置位成功后写进去；这样页面开关状态能自动同步，不需要额外接口。
5. `src/http/api_rest.c` 的 `/api/realtime` — 响应里加 `"led": <0|1>, "ledAddr": <addr>`。

前端兼容性：**后端不加 `led` 字段也能用**——页面指示灯会显示"状态未知"（虚线圆圈），
点开关会收到 `1004` 并弹出"后端未实现 setLed 指令"的提示，不会静默失败。
也就是说现在这版前端接到你现有后端上，除了指示灯，其他功能都能跑。

---

## 五、配置项（`js/config.js`）

| 配置 | 默认 | 说明 |
|---|---|---|
| `MOCK` | `'auto'` | `true` 纯演示 / `false` 只连后端 / `'auto'` 连不上自动降级 |
| `API_BASE` | `'/api'` | 与后端同源就用 `/api`；跨域调试填 `http://192.168.142.146:8080/api`（后端要允许跨域） |
| `POLL_INTERVAL` | `2000` | 实时数据轮询间隔 ms（页面右上还能临时切 1/2/5/10 秒） |
| `REALTIME_POINTS` | `120` | 实时曲线保留点数 |
| `HISTORY_DEFAULT_HOURS` | `24` | 历史查询默认跨度 |
| `ALARM` | tempMax 32 | 阈值默认值（后端 `/api/realtime` 返回真实 `threshold` 时以那个为准） |
| `LED_REG` | `4` | 指示灯寄存器地址（仅界面文案用，实际地址后端定） |

---

## 六、已验证项（无头 Edge + CDP 实测，0 JS 报错）

- 登录页：Tab 切换、空表单/短账号/短密码校验、密码显隐、记住我、token 过期提示
- 注册：账号唯一性（`1002` 标红）、两次密码一致性校验、注册成功回填账号
- 登录：错误密码 → 提示并标红；正确 → 写入登录态并跳 `dashboard.html`
- 看板：温度/湿度/极值/采集点数、实时曲线（ECharts canvas 实测渲染）、数据来源标记（模拟/真实）
- 设备控制：指示灯开关 → 点亮（绿色发光）/ 熄灭、回读状态、采集周期与阈值下发、保持寄存器读取
- 历史记录：时段查询、平均温湿度统计、CSV 导出（带 BOM，Excel 不乱码）
- 日志：级别筛选、WARN/ERROR 计数条
- 轮询暂停/继续、退出登录清登录态、页面隐藏自动停轮询
- 布局：1600/1920/1366/1024/390 五种宽度零横向溢出，底部双栏等高，无文字截断
- 自动降级：后端不可达时亮黄条提示，不是静默失败

---

## 七、需求文档对照

| 需求条款 | 前端落地位置 |
|---|---|
| 3.1 用户管理（注册/登录/账号密码校验） | `index.html` + `js/auth.js` |
| 3.2 温湿度数据采集（周期采集） | 看板轮询 `/api/realtime`；周期可下发 `setPeriod` |
| 3.3 数据展示（实时显示 + 历史查询） | 4 张 KPI 卡 + 实时曲线 + 历史记录查询 |
| 3.4 设备控制（指示灯按钮控制） | 设备控制面板 → 指示灯开关（前端就绪，待后端补 `setLed`） |
| 3.5 数据存储（采集记录/用户/日志） | 历史记录表、日志记录表两个面板 |
| 扩展：登录界面跳转业务页面 | 登录成功 → `dashboard.html`；未登录访问看板自动跳回登录页；token 过期 401 → 回登录页并提示 |
