/* ============================================================================
   mock.js —— 本地模拟数据层
   何时用：
     1) config.js 里 MOCK: true      → 完全不连后端
     2) config.js 里 MOCK: 'auto'    → 后端连不上时自动降级到这里，页面亮黄条提示

   约定：本文件的函数 **直接返回后端 data 里的那个对象**（等价于解包后的 data），
        失败时 throw 一个带 .code 的 Error（模拟后端的业务错误码）。
        这样页面代码在 MOCK 与真实后端下写法完全一致。
   ============================================================================ */

(function () {
  'use strict';

  var CFG = window.APP_CONFIG;

  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  function fmt(d) {
    return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' ' +
           pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }
  function rnd(a, b) { return a + Math.random() * (b - a); }

  function fail(code, msg) {
    var e = new Error(msg);
    e.code = code;
    throw e;
  }

  /* ------------------------------------------------------------------
     内存"数据库"：用户信息表 / 日志记录表 / 采集统计
     ------------------------------------------------------------------ */
  var UNIQ = ['', '综合楼一层', '综合楼二层', '地下车库', '顶层会所'];

  var DB = {
    /* 用户信息表（对应后端 users 表，密码在真后端里是 salt$sha256，这里只演示流程） */
    users: { admin: { password: '123456', role: 'admin', uid: 1 } },
    nextUid: 2,

    /* 登录会话（对应后端 sessions 表，TTL 7200s） */
    sessions: {},

    /* 运行状态 */
    running: true,
    startAt: Date.now(),
    period: 2000,
    threshold: CFG.ALARM.tempMax,
    led: 0,                 /* 指示灯状态：1 亮 / 0 灭 */
    ledAddr: CFG.LED_REG,
    count: 0,
    temp: CFG.MOCK_INIT.temp,
    humi: CFG.MOCK_INIT.humi,
    tempMax: CFG.MOCK_INIT.temp,
    tempMin: CFG.MOCK_INIT.temp,
    lastCollect: new Date(),

    /* 日志记录表：倒序，上限 200 条 */
    logs: [],
    /* 历史采集记录表 */
    history: []
  };

  function log(level, module, message) {
    DB.logs.unshift({ time: fmt(new Date()), level: level, module: module, message: message });
    if (DB.logs.length > 200) DB.logs.length = 200;
  }

  /* 预置一批日志，让页面一打开不是空的 */
  (function seedLogs() {
    var t0 = Date.now();
    var seeds = [
      ['INFO', 'web', 'webserver 启动，监听 0.0.0.0:8080'],
      ['INFO', 'shm', '共享内存已挂接（SHM_MAGIC 校验通过）'],
      ['INFO', 'mq', '消息队列 /modbus_sensor_mq 就绪'],
      ['INFO', 'collector', '采集进程启动，周期 2000 ms'],
      ['INFO', 'modbus', 'TCP 连接从机 127.0.0.1:5020 成功，slaveId=1'],
      ['INFO', 'db', 'SQLite sensor.db 已打开（CREATE TABLE IF NOT EXISTS）'],
      ['DEBUG', 'shm', '读锁获取成功，耗时 1 ms'],
      ['INFO', 'db', '采集记录已写入 sensor_data 表']
    ];
    for (var i = seeds.length - 1; i >= 0; i--) {
      var d = new Date(t0 - (seeds.length - i) * 4000);
      DB.logs.unshift({ time: fmt(d), level: seeds[i][0], module: seeds[i][1], message: seeds[i][2] });
    }
  })();

  /* ------------------------------------------------------------------
     采集模拟：温度/湿度随机游走 + 极值统计 + 定时入库
     ------------------------------------------------------------------ */
  function tick() {
    if (!DB.running) return;
    var now = new Date();
    var hour = now.getHours() + now.getMinutes() / 60;
    /* 白天高、夜里低的日内趋势 + 噪声 */
    var trend = 2.2 * Math.sin((hour - 9) / 24 * Math.PI * 2);
    DB.temp = DB.temp * 0.86 + (24.6 + trend + rnd(-0.28, 0.28)) * 0.14;
    DB.humi = DB.humi * 0.86 + (56 - trend * 2.4 + rnd(-0.9, 0.9)) * 0.14;
    DB.temp = Math.round(DB.temp * 10) / 10;
    DB.humi = Math.round(DB.humi * 10) / 10;
    if (DB.temp > DB.tempMax) DB.tempMax = DB.temp;
    if (DB.temp < DB.tempMin) DB.tempMin = DB.temp;
    DB.count += 1;
    DB.lastCollect = now;

    if (DB.temp >= DB.threshold) {
      log('WARN', 'collector', '温度 ' + DB.temp + ' ℃ 超过阈值 ' + DB.threshold + ' ℃');
    }
    /* 每 5 个采集点写一条历史记录，模拟入库频率 */
    if (DB.count % 5 === 0) {
      DB.history.push({ time: fmt(now), temp: DB.temp, humi: DB.humi });
      if (DB.history.length > 20000) DB.history.splice(0, DB.history.length - 20000);
    }
  }
  setInterval(tick, 2000);

  /* 预生成过去 N 小时的历史（每 5 分钟一个点），让"查询历史"一打开就有数据 */
  (function seedHistory() {
    var now = Date.now();
    var pts = 24 * 12;                     /* 24 小时 × 12 = 288 点 */
    for (var i = pts; i >= 0; i--) {
      var d = new Date(now - i * 5 * 60 * 1000);
      var hour = d.getHours() + d.getMinutes() / 60;
      var trend = 2.2 * Math.sin((hour - 9) / 24 * Math.PI * 2);
      DB.history.push({
        time: fmt(d),
        temp: Math.round((24.6 + trend + rnd(-0.6, 0.6)) * 10) / 10,
        humi: Math.round((56 - trend * 2.4 + rnd(-2.2, 2.2)) * 10) / 10
      });
    }
    DB.count = DB.history.length;
    var temps = DB.history.map(function (x) { return x.temp; });
    DB.tempMax = Math.max.apply(null, temps);
    DB.tempMin = Math.min.apply(null, temps);
  })();

  /* ------------------------------------------------------------------
     对外模拟实现
     ------------------------------------------------------------------ */
  window.Mock = {

    register: function (username, password) {
      username = (username || '').trim();
      if (!username || !password) fail(1003, '用户名或密码不能为空');
      if (DB.users[username]) fail(1002, '用户名已存在');
      DB.users[username] = { password: password, role: 'user', uid: DB.nextUid++ };
      log('INFO', 'web', '新用户注册成功：' + username + '（写入用户信息表）');
      return { uid: DB.users[username].uid, username: username };
    },

    login: function (username, password) {
      username = (username || '').trim();
      if (!username || !password) fail(1003, '用户名或密码不能为空');
      var u = DB.users[username];
      if (!u || u.password !== password) {
        log('WARN', 'web', '登录失败：账号或密码错误（' + (username || '空账号') + '）');
        fail(1001, '用户名或密码错误');
      }
      var token = 'mock-' + Math.random().toString(36).slice(2, 10) + Date.now().toString(36);
      DB.sessions[token] = { username: username, expire: Date.now() + 7200 * 1000 };
      log('INFO', 'web', '用户 ' + username + ' 登录成功，会话已写入 sessions 表（TTL 7200s）');
      return { username: username, role: u.role, uid: u.uid, token: token };
    },

    /* 实时数据：等价于 webserver 读共享内存 */
    realtime: function () {
      if (!DB.running) fail(2001, '采集进程未运行（共享内存不存在）');
      return {
        temp: DB.temp,
        humi: DB.humi,
        tempMax: Math.round(DB.tempMax * 10) / 10,
        tempMin: Math.round(DB.tempMin * 10) / 10,
        count: DB.count,
        slaveId: 1,
        online: true,
        collectTime: fmt(DB.lastCollect),
        period: DB.period,
        threshold: DB.threshold,
        led: DB.led,
        ledAddr: DB.ledAddr,
        source: {
          name: 'modbus-simulator',
          simulated: true,
          target: 'tcp 127.0.0.1:5020 从机1'
        }
      };
    },

    /* 历史记录：等价于查 SQLite 温湿度采集记录表 */
    history: function (hours) {
      hours = hours || CFG.HISTORY_DEFAULT_HOURS;
      var since = Date.now() - hours * 3600 * 1000;
      var list = DB.history.filter(function (x) {
        return new Date(x.time.replace(/-/g, '/')).getTime() >= since;
      });
      if (list.length > 1000) list = list.slice(list.length - 1000);
      return { hours: hours, list: list };
    },

    /* 指令下发：等价于 webserver 投递消息队列 */
    command: function (action, params) {
      params = params || {};
      if (action === 'setPeriod') {
        var ms = parseInt(params.period, 10);
        if (!(ms >= 100 && ms <= 60000)) fail(1004, '采集周期需在 100~60000 ms 之间');
        DB.period = ms;
        log('INFO', 'mq', 'CMD_SET_INTERVAL 已下发，采集周期改为 ' + ms + ' ms');
        return { action: action, period: ms, send_rc: 0 };
      }
      if (action === 'setThreshold') {
        var v = Number(params.threshold);
        if (!isFinite(v)) fail(1004, '阈值参数非法');
        DB.threshold = v;
        log('INFO', 'mq', 'CMD_SET_THRESHOLD 已下发，温度告警阈值改为 ' + v.toFixed(1) + ' ℃');
        return { action: action, threshold: v, send_rc: 0 };
      }
      if (action === 'setLed') {
        var on = Number(params.on) ? 1 : 0;
        DB.led = on;
        log('INFO', 'mq', 'CMD_SET_LED 已下发：指示灯' + (on ? '置位（点亮）' : '复位（熄灭）') +
            '，寄存器 ' + DB.ledAddr);
        log('INFO', 'modbus', '写从机线圈/寄存器 addr=' + DB.ledAddr + ' value=' + on + ' 成功');
        return { action: action, led: on, addr: DB.ledAddr, send_rc: 0 };
      }
      if (action === 'readRegister') {
        var addr = parseInt(params.addr, 10) || 0;
        var count = parseInt(params.count, 10) || 2;
        log('DEBUG', 'mq', 'CMD_QUERY 已下发，读取寄存器 addr=' + addr + ' count=' + count);
        return {
          action: action, addr: addr, count: count, regsAddr: addr,
          regs: [Math.round(DB.temp * 10), Math.round(DB.humi * 10)].slice(0, count)
        };
      }
      if (action === 'restart') {
        log('WARN', 'collector', '收到 restart 指令（演示版只在日志记录，不真正重启进程）');
        return { action: action, restart: true, note: '演示实现：仅记录日志' };
      }
      fail(1004, '未知指令：' + action);
    },

    logs: function () {
      return { list: DB.logs.slice(0, 200) };
    },

    status: function () {
      return {
        collectOnline: DB.running,
        webOnline: true,
        modbusOnline: DB.running,
        period: DB.period,
        uptime: Math.round((Date.now() - DB.startAt) / 1000),
        lastCollectTs: Math.floor(DB.lastCollect.getTime() / 1000),
        deviceStatus: DB.running ? 1 : 0,
        slaveId: 1,
        led: DB.led,
        source: DB.running ? {
          name: 'modbus-simulator',
          simulated: true,
          target: 'tcp 127.0.0.1:5020 从机1'
        } : null
      };
    }
  };
})();
