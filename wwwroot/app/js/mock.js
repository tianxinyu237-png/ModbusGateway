/* ============================================================
   mock.js —— 模拟数据层
   在 C 语言 webserver 完成之前，用浏览器本地数据模拟：
     温湿度传感器 → 采集进程 → 共享内存 → webserver → 浏览器
   后端完成后把 config.js 中 MOCK 改为 false 即可无缝切换
   ============================================================ */

(function () {
  var CFG = window.APP_CONFIG;
  var state = {
    temp: CFG.MOCK_INIT.temp,
    humi: CFG.MOCK_INIT.humi,
    tempMax: CFG.MOCK_INIT.temp,
    tempMin: CFG.MOCK_INIT.temp,
    count: 0,
    threshold: CFG.THRESHOLD.tempMax,
    period: 2000,
    online: true,
    startAt: Date.now()
  };

  /* 随机游走：模拟真实传感器读数的缓慢漂移 */
  function walk(base, step, min, max) {
    var v = base + (Math.random() - 0.5) * step;
    if (v < min) v = min;
    if (v > max) v = max;
    return v;
  }

  /* 用户表：模拟 SQLite 用户信息表 */
  var users = [
    { username: 'admin', password: '123456', role: 'admin' }
  ];

  function pad(n) { return n < 10 ? '0' + n : '' + n; }

  function fmt(d) {
    d = d || new Date();
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' +
      pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
  }

  /* 模拟日志记录表 */
  var logs = [];
  function pushLog(level, module, message) {
    logs.unshift({ time: fmt(), level: level, module: module, message: message });
    if (logs.length > 200) logs.pop();
  }

  /* 生成历史数据：模拟 SQLite 温湿度采集记录表 */
  function genHistory(hours) {
    var now = Date.now();
    var stepMs = 5 * 60 * 1000;                 // 5 分钟一个采样点
    var n = Math.max(2, Math.round(hours * 3600 * 1000 / stepMs));
    var list = [];
    var t = CFG.MOCK_INIT.temp, h = CFG.MOCK_INIT.humi;
    for (var i = n; i >= 0; i--) {
      var ts = now - i * stepMs;
      var hour = new Date(ts).getHours();
      // 叠加日周期波动，让曲线更像真实环境
      var dayWave = Math.sin((hour - 6) / 24 * Math.PI * 2) * 3.2;
      t = walk(t, 0.9, 12, 34);
      h = walk(h, 1.6, 30, 88);
      list.push({
        ts: ts,
        time: fmt(new Date(ts)),
        temp: +(t + dayWave).toFixed(2),
        humi: +h.toFixed(2)
      });
    }
    return list;
  }

  pushLog('INFO',  'modbus',   'connected to slave 0x01 @ 192.168.1.100:502');
  pushLog('INFO',  'collect',  '采集进程启动完成 (pid=2041, daemon)');
  pushLog('INFO',  'shm',      'shared memory attach success, key=0x4d474754');
  pushLog('INFO',  'mq',       'message queue opened: /mq_modbus_cmd');
  pushLog('INFO',  'webserver','listen on 0.0.0.0:8080, epoll ET mode');
  pushLog('DEBUG', 'epoll',    'epoll_create1(EPOLL_CLOEXEC) = 4, events=1024');

  window.Mock = {
    /* ---------- 用户系统 ---------- */
    register: function (username, password) {
      for (var i = 0; i < users.length; i++) {
        if (users[i].username === username) {
          return { code: 1002, message: '用户名已存在' };
        }
      }
      users.push({ username: username, password: password, role: 'user' });
      pushLog('INFO', 'webserver', '新用户注册: ' + username + ' → 写入用户信息表');
      return { code: 0, message: '注册成功' };
    },

    login: function (username, password) {
      for (var i = 0; i < users.length; i++) {
        if (users[i].username === username && users[i].password === password) {
          pushLog('INFO', 'webserver', '用户登录成功: ' + username);
          return {
            code: 0, message: '登录成功',
            data: { username: username, role: users[i].role, token: 'mock-' + Date.now() }
          };
        }
      }
      pushLog('WARN', 'webserver', '登录失败: ' + username + ' (密码错误或用户不存在)');
      return { code: 1001, message: '用户名或密码错误' };
    },

    /* ---------- 实时数据（对应共享内存读取） ---------- */
    realtime: function () {
      state.temp = +walk(state.temp, 0.35, 12, 34).toFixed(2);
      state.humi = +walk(state.humi, 0.8, 30, 88).toFixed(2);
      state.count++;
      if (state.temp > state.tempMax) state.tempMax = state.temp;
      if (state.temp < state.tempMin) state.tempMin = state.temp;

      if (state.temp > state.threshold) {
        pushLog('WARN', 'collect', '温度超阈值: ' + state.temp + '℃ > ' + state.threshold + '℃');
        state.threshold = state.threshold; // 保持，避免刷屏
      }

      return {
        code: 0,
        data: {
          temp: state.temp,
          humi: state.humi,
          tempMax: +state.tempMax.toFixed(2),
          tempMin: +state.tempMin.toFixed(2),
          count: state.count,
          slaveId: 1,
          online: state.online,
          collectTime: fmt()
        }
      };
    },

    /* ---------- 历史数据 ---------- */
    history: function (hours) {
      return { code: 0, data: { list: genHistory(hours || 24) } };
    },

    /* ---------- 指令下发（对应消息队列投递） ---------- */
    command: function (action, params) {
      var msg = '';
      if (action === 'setPeriod') {
        state.period = params.period;
        msg = '已下发采集周期指令 → 消息队列 /mq_modbus_cmd，period=' + params.period + 'ms';
        pushLog('INFO', 'mq', 'mq_send(cmd=SET_PERIOD, period=' + params.period + ')');
      } else if (action === 'setThreshold') {
        state.threshold = params.threshold;
        msg = '已下发温度告警阈值指令，threshold=' + params.threshold + '℃';
        pushLog('INFO', 'mq', 'mq_send(cmd=SET_THRESHOLD, value=' + params.threshold + ')');
      } else if (action === 'readRegister') {
        msg = '已下发读取寄存器指令 addr=' + params.addr + ' count=' + params.count +
              '，返回 ' + params.count + ' 个寄存器值';
        pushLog('INFO', 'modbus', 'read_holding_registers(addr=' + params.addr + ', count=' + params.count + ')');
      } else if (action === 'restart') {
        msg = '已下发重启采集进程指令，采集进程将重新加载配置';
        pushLog('WARN', 'collect', '收到重启指令，采集进程准备退出并重新拉起');
      }
      return { code: 0, message: msg };
    },

    /* ---------- 日志记录表 ---------- */
    logs: function () {
      return { code: 0, data: { list: logs.slice(0, 60) } };
    },

    /* ---------- 运行状态 ---------- */
    status: function () {
      return {
        code: 0,
        data: {
          collectOnline: state.online,
          webOnline: true,
          modbusOnline: state.online,
          period: state.period,
          uptime: Math.round((Date.now() - state.startAt) / 1000)
        }
      };
    }
  };
})();
