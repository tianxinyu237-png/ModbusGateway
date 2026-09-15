工程自测工具与接口说明（hermes 加的，可以直接删）
=======================================================

【先记住】make 必须在工程目录里跑：
    cd ~/network/lianxi && make
在家目录 ~ 里直接敲 make 会报"没有指明目标并且找不到 makefile"。

一键全流程验证（推荐）
    cd ~/network/lianxi && bash tools/run_all_tests.sh
    7 个阶段：编译 -> 数据库自测 -> 起从机+采集+web -> REST接口 -> 老接口/消息队列
              -> web运行时 -> 前端静态托管+数据库汇总

-------------------------------------------------------
〇、设备配置（接真实设备只改配置，不用改代码/重编译）
-------------------------------------------------------
配置文件：工程根目录 collector.conf（# 是注释，支持行内注释）
查看当前生效配置：  ./collector.out --show-conf

接网口设备（Modbus TCP）：
    ./collector.out --host 192.168.1.100 --port 502 --slave 1 --real \
                    --source "车间1号温湿度计" --daemon
接串口设备（RS485 / Modbus RTU）：
    ./collector.out --transport rtu --serial /dev/ttyUSB0 --baud 9600 --daemon

关键配置项：
    transport=tcp|rtu        host/port        设备地址（标准Modbus TCP端口502）
    slave=1                  从机地址(站号)
    reg_temp=0 reg_humi=1    温湿度寄存器地址
    scale_temp=10 scale_humi=10   真实值 = 寄存器原始值 ÷ 该值
    interval_ms=2000 threshold=32.0   采集周期 / 温度告警阈值
    source_name / source_simulated    数据来源标记（1=模拟数据 0=真实设备）

数据来源标记会显示在三个地方，答辩/验收时一眼能看出是不是真实数据：
    · 网页看板顶部橙色横幅 + "系统运行状态"里的"数据来源"一行
    · ./collector.out --status
    · SQLite logs 表 + syslog（启动时会写一条）

-------------------------------------------------------
一、网页端页面（ModbusGateway 前端，静态托管在 wwwroot/app/）
-------------------------------------------------------
    登录/注册： http://<虚拟机IP>:8080/app/index.html
    数据看板： http://<虚拟机IP>:8080/app/dashboard.html   （演示账号 admin/123456，需先注册）
    实时监控（老页面）： http://<虚拟机IP>:8080/realtime.html
启动顺序：python3 tools/modbus_slave.py  →  ./collector.out  →  ./thttpd.out 8080
（config.js 里 MOCK 已切成 false，直连自研 C 后端；想纯前端演示就改回 true）

-------------------------------------------------------
二、RESTful 接口（前端用的，统一响应 {"code":0,"message":"ok","data":{...}}）
    除注册/登录外都需要请求头 Authorization: Bearer <token>，未登录返回 HTTP 401
-------------------------------------------------------
1  POST /api/register        {"username":"xx","password":"yy"}
                             -> code 0 成功 / 1002 用户名已存在 / 1003 参数为空
                             密码在库里存的是 salt$sha256(salt+password)，不存明文
2  POST /api/login           {"username":"xx","password":"yy"}
                             -> code 0 + data{username,role,uid,token} / 1001 用户名或密码错误
3  POST /api/logout          -> code 0（token 立即失效）
4  GET  /api/realtime        -> data{temp,humi,tempMax,tempMin,count,slaveId,online,collectTime,period,threshold}
                               （读共享内存，加读锁；采集进程没跑时 code 2001）
5  GET  /api/history?hours=24 -> data{hours,list:[{time,temp,humi}]}（SQLite，时间升序）
6  POST /api/command         {"action":"setPeriod","period":2000}     改采集周期(100~60000ms)
                             {"action":"setThreshold","threshold":32} 改温度告警阈值(℃)
                             {"action":"readRegister","addr":0,"count":2} 读保持寄存器并取回值
                             {"action":"restart"}                     重启（当前仅记录，见下方说明）
7  GET  /api/logs            -> data{list:[{time,level,module,message}]}（分级日志表）
8  GET  /api/status          -> data{collectOnline,webOnline,modbusOnline,period,uptime}

老接口（realtime.html 和命令行用，返回裸JSON，没被破坏）：
    GET /api?cmd=realtime / ?cmd=interval&sec=N / ?cmd=history&limit=N

-------------------------------------------------------
三、架构说明（数据怎么流动）
-------------------------------------------------------
    浏览器 --HTTP--> thttpd/custom_handle --读共享内存(带读写锁)--> 采集进程
                                          --投递消息队列--> 采集进程
                                          --读写 SQLite--> 历史/用户/日志
    采集进程 --Modbus TCP--> 从机(模拟器或真实设备)，把实时值写进共享内存 + 落库

    共享内存 shm_sensor_data_t 里带了 pthread 进程间读写锁：
      采集进程写数据时持写锁，web 读时持读锁（带超时，拿不到锁就报错，不会死等）
      结构体开头有 SHM_MAGIC，web 挂接时先校验，避免新旧结构体混用读到垃圾

    消息队列指令码（src/ipc/mq.h）：
      CMD_QUERY(1)         param 高16位=起始地址 低16位=个数
      CMD_SET_INTERVAL(2)  param=周期ms
      CMD_SET_THRESHOLD(3) param=阈值*100（3250 -> 32.5℃）
      CMD_RESTART(4)       无参数

-------------------------------------------------------
四、测试工具
-------------------------------------------------------
tools/modbus_slave.py    Modbus TCP 从机模拟器（纯python3标准库，不需要 pymodbus/diagslave）
                         监听 0.0.0.0:5020，寄存器0=温度x10，寄存器1=湿度x10，值随时间漂移
tools/db_selftest.c      db.c 自测：SHA-256标准向量 / 注册 / 重复注册 / 登录对错 /
                         插记录 / 统计 / 按跨度查历史 / 分级日志（27项）
tools/tests_api_rest.py  REST 8接口 + token鉴权 + 401/404（32项）
tools/test_api.py        老接口 + 共享内存/消息队列集成（8项）
tools/tests_web.py       web 运行时：静态/登录/求和/404/405/路径穿越/兜底/新老接口（27项）
tools/mock_server.py     Python 版模拟后端（前端 MOCK 联调用，非必需）
tools/run_all_tests.sh   一键跑全部

-------------------------------------------------------
五、环境依赖（已装好，换机器要重装）
-------------------------------------------------------
  - cJSON 1.7.18 -> /usr/local/include/cjson/cJSON.h, /usr/local/lib/libcjson.so
  - libmodbus 头文件是扁平的 /usr/include/modbus.h；写 <libmodbus/libmodbus.h> 也能编（有shim）
  - Makefile 加 -lrt（18.04 的 mq_open 在 librt 里）+ -I.（引号include要能找到工程根）
  - 密码哈希是自带的纯C SHA-256，不依赖 openssl

-------------------------------------------------------
六、还没做的（对应架构图上的后续阶段）
-------------------------------------------------------
  - epoll reactor（现在是 accept + 每连接一线程）
  - 采集进程 daemon 化（fork+setsid）、异常自愈/自动拉起 → 所以 /api/command 的 restart 只记录不真重启
  - 日志按天轮转 + 写 syslog（现在只写 SQLite logs 表）
  - 前端"记住我"、密码修改、用户管理页
