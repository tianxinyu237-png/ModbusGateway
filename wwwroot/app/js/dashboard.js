/* ============================================================
   dashboard.js —— 数据监控看板逻辑
   对应流程图中的两条链路：
     上行：采集进程 → 共享内存 → webserver → 浏览器（实时曲线）
     下行：浏览器 → webserver → 消息队列 → 采集进程（指令下发）
   ============================================================ */

(function () {
  var CFG = window.APP_CONFIG;
  var loading = document.getElementById('loading');

  /* ---------- 0. 登录校验 ---------- */
  var user = API.Session.get();
  if (!user || !user.username) {
    location.replace('index.html');
    return;
  }
  document.getElementById('uname').textContent = user.username;
  document.getElementById('avatar').textContent = user.username.charAt(0).toUpperCase();
  if (CFG.MOCK) document.getElementById('mock-bar').style.display = 'flex';

  document.getElementById('btn-logout').addEventListener('click', function () {
    API.logout();
  });

  /* ---------- 1. ECharts 可用性检查（CDN 失败时降级到本地文件） ---------- */
  var charts = { realtime: null, history: null };

  function ensureECharts(cb) {
    if (typeof echarts !== 'undefined') { cb(true); return; }
    var s = document.createElement('script');
    s.src = 'js/echarts.min.js';
    s.onload = function () { cb(typeof echarts !== 'undefined'); };
    s.onerror = function () { cb(false); };
    document.head.appendChild(s);
  }

  function emptyState(el, text) {
    el.innerHTML = '<div class="empty">' + text + '</div>';
  }

  /* ---------- 2. 图表初始化 ---------- */
  var RT_MAX = CFG.REALTIME_POINTS;
  var rtTime = [], rtTemp = [], rtHumi = [];

  function baseAxis() {
    return {
      axisLine: { lineStyle: { color: '#e3e8ef' } },
      axisLabel: { color: '#8a97a8', fontSize: 11 },
      splitLine: { lineStyle: { color: '#f1f4f9' } }
    };
  }

  function initRealtime() {
    var el = document.getElementById('chart-realtime');
    charts.realtime = echarts.init(el);
    charts.realtime.setOption({
      tooltip: {
        trigger: 'axis',
        backgroundColor: 'rgba(44,62,80,.92)',
        borderWidth: 0,
        textStyle: { color: '#fff', fontSize: 12 }
      },
      legend: {
        data: ['温度(℃)', '湿度(%RH)'],
        right: 10, top: 0,
        textStyle: { color: '#5a6b80', fontSize: 12 }
      },
      grid: { left: 52, right: 56, top: 40, bottom: 34 },
      xAxis: Object.assign({ type: 'category', boundaryGap: false, data: [] }, baseAxis()),
      yAxis: [
        {
          type: 'value', name: '温度(℃)', nameTextStyle: { color: '#c0397b', fontSize: 11 },
          axisLabel: { color: '#c0397b', fontSize: 11 },
          axisLine: { show: false }, splitLine: { lineStyle: { color: '#f1f4f9' } }
        },
        {
          type: 'value', name: '湿度(%RH)', nameTextStyle: { color: '#3478f6', fontSize: 11 },
          axisLabel: { color: '#3478f6', fontSize: 11 },
          axisLine: { show: false }, splitLine: { show: false }
        }
      ],
      series: [
        {
          name: '温度(℃)', type: 'line', smooth: true, symbol: 'none',
          yAxisIndex: 0, data: [],
          lineStyle: { width: 2, color: '#c0397b' },
          areaStyle: {
            color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
              { offset: 0, color: 'rgba(192,57,123,.22)' },
              { offset: 1, color: 'rgba(192,57,123,0)' }
            ])
          }
        },
        {
          name: '湿度(%RH)', type: 'line', smooth: true, symbol: 'none',
          yAxisIndex: 1, data: [],
          lineStyle: { width: 2, color: '#3478f6' },
          areaStyle: {
            color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
              { offset: 0, color: 'rgba(52,120,246,.18)' },
              { offset: 1, color: 'rgba(52,120,246,0)' }
            ])
          }
        }
      ]
    });
  }

  function initHistory() {
    var el = document.getElementById('chart-history');
    charts.history = echarts.init(el);
    charts.history.setOption({
      tooltip: {
        trigger: 'axis',
        backgroundColor: 'rgba(44,62,80,.92)',
        borderWidth: 0,
        textStyle: { color: '#fff', fontSize: 12 }
      },
      legend: {
        data: ['温度(℃)', '湿度(%RH)'],
        right: 10, top: 0,
        textStyle: { color: '#5a6b80', fontSize: 12 }
      },
      grid: { left: 52, right: 56, top: 40, bottom: 62 },
      dataZoom: [
        { type: 'inside', start: 0, end: 100 },
        {
          type: 'slider', height: 18, bottom: 12,
          borderColor: '#e3e8ef', fillerColor: 'rgba(52,120,246,.12)',
          handleStyle: { color: '#3478f6' },
          textStyle: { color: '#8a97a8', fontSize: 11 }
        }
      ],
      xAxis: Object.assign({ type: 'category', boundaryGap: false, data: [] }, baseAxis()),
      yAxis: [
        {
          type: 'value', name: '温度(℃)', nameTextStyle: { color: '#c0397b', fontSize: 11 },
          axisLabel: { color: '#c0397b', fontSize: 11 },
          axisLine: { show: false }, splitLine: { lineStyle: { color: '#f1f4f9' } }
        },
        {
          type: 'value', name: '湿度(%RH)', nameTextStyle: { color: '#3478f6', fontSize: 11 },
          axisLabel: { color: '#3478f6', fontSize: 11 },
          axisLine: { show: false }, splitLine: { show: false }
        }
      ],
      series: [
        { name: '温度(℃)', type: 'line', smooth: true, symbol: 'none', yAxisIndex: 0, data: [], lineStyle: { width: 2, color: '#c0397b' } },
        { name: '湿度(%RH)', type: 'line', smooth: true, symbol: 'none', yAxisIndex: 1, data: [], lineStyle: { width: 2, color: '#3478f6' } }
      ]
    });
  }

  /* ---------- 3. 状态灯与卡片 ---------- */
  function setLed(id, on, warn) {
    var el = document.getElementById(id);
    el.className = 'led' + (on ? (warn ? ' warn' : ' on') : ' off');
  }

  function fmtDuration(sec) {
    var h = Math.floor(sec / 3600), m = Math.floor(sec % 3600 / 60), s = sec % 60;
    return (h > 0 ? h + ' 小时 ' : '') + m + ' 分 ' + s + ' 秒';
  }

  function updateKpi(d) {
    document.getElementById('k-temp').innerHTML = d.temp.toFixed(1) + '<small>℃</small>';
    document.getElementById('k-humi').innerHTML = d.humi.toFixed(1) + '<small>%RH</small>';
    document.getElementById('k-peak').textContent = d.tempMax.toFixed(1) + ' / ' + d.tempMin.toFixed(1) + ' ℃';
    document.getElementById('k-count').textContent = d.count;

    var over = d.temp > CFG.THRESHOLD.tempMax;
    document.getElementById('k-temp-foot').textContent =
      over ? '⚠ 已超过告警阈值 ' + CFG.THRESHOLD.tempMax + '℃' : '状态正常，采集中';
    document.getElementById('k-humi-foot').textContent = '传感器 0x0' + (d.slaveId || 1) + ' 在线';
    document.getElementById('i-last').textContent = d.collectTime || '--';
  }

  /* ---------- 4. 实时数据轮询 ---------- */
  var timer = null, paused = false, lastStatus = null;

  function pushRealtime(d) {
    var label = d.collectTime ? d.collectTime.substring(11) : new Date().toLocaleTimeString('zh-CN', { hour12: false });
    rtTime.push(label);
    rtTemp.push(d.temp);
    rtHumi.push(d.humi);
    if (rtTime.length > RT_MAX) { rtTime.shift(); rtTemp.shift(); rtHumi.shift(); }

    if (charts.realtime) {
      charts.realtime.setOption({
        xAxis: { data: rtTime },
        series: [{ data: rtTemp }, { data: rtHumi }]
      });
    }
  }

  var lastProblem = '';
  /* 后端明确报错时（比如采集进程没运行），把原因写到卡片上，
     而不是静默什么都不做 —— 不然用户只看到 "--" 完全不知道为什么 */
  function showProblem(msg) {
    if (msg === lastProblem) return;
    lastProblem = msg;
    var a = document.getElementById('k-temp-foot');
    var b = document.getElementById('k-humi-foot');
    if (a) a.textContent = msg;
    if (b) b.textContent = msg;
    setLed('led-collect', false);
    console.warn('后端返回：' + msg);
  }
  function clearProblem() {
    if (!lastProblem) return;
    lastProblem = '';
    var a = document.getElementById('k-temp-foot');
    var b = document.getElementById('k-humi-foot');
    if (a) a.textContent = '数据正常刷新中';
    if (b) b.textContent = '数据正常刷新中';
  }

  function tick() {
    API.realtime().then(function (res) {
      if (res.code !== 0) {
        showProblem(res.message || ('后端错误码 ' + res.code));
        return;
      }
      clearProblem();
      var d = res.data;
      updateKpi(d);
      pushRealtime(d);
    }).catch(function (err) {
      setLed('led-web', false);
      showProblem('与 web 服务通信失败：' + err.message);
      console.warn('实时数据获取失败：', err.message);
    });
  }

  function startPoll(interval) {
    if (timer) clearInterval(timer);
    if (paused) return;
    timer = setInterval(tick, interval);
  }

  /* 刷新间隔切换 */
  document.getElementById('poll-select').addEventListener('change', function (e) {
    startPoll(Number(e.target.value));
  });

  /* 暂停 / 继续 */
  var btnPause = document.getElementById('btn-pause');
  btnPause.addEventListener('click', function () {
    paused = !paused;
    btnPause.textContent = paused ? '继续' : '暂停';
    btnPause.className = paused ? 'btn' : 'btn sec';
    startPoll(Number(document.getElementById('poll-select').value));
  });

  /* ---------- 5. 运行状态 ---------- */
  function refreshStatus() {
    API.status().then(function (res) {
      if (res.code !== 0) return;
      var s = res.data;
      lastStatus = s;
      setLed('led-web', s.webOnline);
      setLed('led-collect', s.collectOnline);
      setLed('led-modbus', s.modbusOnline);
      document.getElementById('i-period').textContent = s.period + ' ms';
      document.getElementById('i-uptime').textContent = fmtDuration(s.uptime);
      document.getElementById('k-count-foot').textContent = '采集周期 ' + s.period + ' ms';
      document.getElementById('cmd-period').value = s.period;
      if (s.slaveId) document.getElementById('i-slave').textContent = '站号 ' + s.slaveId;

      /* 数据来源：模拟器 or 真实设备（后端 /api/status 的 source 字段） */
      if (s.source) {
        var sim = !!s.source.simulated;
        var srcEl = document.getElementById('i-source');
        if (srcEl) srcEl.textContent = s.source.name + (sim ? '（模拟数据）' : '（真实设备）');
        var bar = document.getElementById('mock-bar');
        if (bar) {
          if (sim) {
            bar.style.display = 'flex';
            bar.innerHTML =
              '<span>⚠ 当前数据来源：<b>' + s.source.name + '</b>（' + (s.source.target || '-') +
              '）—— <b>这是模拟数据，不是真实传感器</b>。</span>' +
              '<span>接真实设备：改 <code>collector.conf</code>，或用 ' +
              '<code>./collector.out --host 设备IP --port 502 --real</code> 重启采集进程</span>';
          } else {
            bar.style.display = 'none';
          }
        }
      }
    });
  }

  /* 顶部时钟 */
  setInterval(function () {
    var d = new Date();
    document.getElementById('st-time').textContent =
      d.toLocaleTimeString('zh-CN', { hour12: false });
  }, 1000);

  /* ---------- 6. 历史查询 ---------- */
  document.getElementById('btn-query').addEventListener('click', queryHistory);

  function queryHistory() {
    var hours = Number(document.getElementById('hist-range').value);
    var el = document.getElementById('chart-history');
    if (charts.history) charts.history.showLoading({ text: '查询中…', color: '#3478f6', textColor: '#5a6b80' });

    API.history(hours).then(function (res) {
      if (charts.history) charts.history.hideLoading();
      if (res.code !== 0) return;
      var list = (res.data && res.data.list) || [];
      if (!list.length) {
        if (charts.history) charts.history.clear();
        emptyState(el, '所选时间范围内暂无采集记录');
        return;
      }
      // 时间跨度较大时只显示 月-日 时:分
      var labels = list.map(function (it) {
        return hours > 48 ? it.time.substring(5, 16) : it.time.substring(11, 16);
      });
      charts.history.setOption({
        xAxis: { data: labels },
        series: [
          { data: list.map(function (it) { return it.temp; }) },
          { data: list.map(function (it) { return it.humi; }) }
        ]
      });
    }).catch(function (err) {
      if (charts.history) charts.history.hideLoading();
      emptyState(el, '查询失败：' + err.message);
    });
  }

  /* ---------- 7. 指令下发 ---------- */
  var btnSend = document.getElementById('btn-send');
  var cmdResult = document.getElementById('cmd-result');

  function printCmd(text) {
    cmdResult.innerHTML = text;
  }

  btnSend.addEventListener('click', function () {
    var period = Number(document.getElementById('cmd-period').value);
    var threshold = Number(document.getElementById('cmd-threshold').value);
    var addr = Number(document.getElementById('cmd-addr').value);
    var count = Number(document.getElementById('cmd-count').value);

    if (!period || period < 200) { printCmd('<b>参数错误：</b>采集周期不能小于 200 ms'); return; }
    if (count < 1 || count > 125) { printCmd('<b>参数错误：</b>寄存器数量范围为 1 ~ 125'); return; }

    btnSend.disabled = true;
    printCmd('正在通过 webserver 投递指令到消息队列…');

    Promise.all([
      API.command('setPeriod', { period: period }),
      API.command('setThreshold', { threshold: threshold }),
      API.command('readRegister', { addr: addr, count: count })
    ]).then(function (rs) {
      var lines = ['<b>指令下发成功</b>（' + new Date().toLocaleTimeString('zh-CN', { hour12: false }) + '）'];
      rs.forEach(function (r) { lines.push('· ' + (r.message || 'ok')); });
      printCmd(lines.join('<br>'));
      refreshStatus();
      refreshLogs();
    }).catch(function (err) {
      printCmd('<b>下发失败：</b>' + err.message);
    }).then(function () {
      btnSend.disabled = false;
    });
  });

  /* ---------- 8. 日志 ---------- */
  function refreshLogs() {
    API.logs().then(function (res) {
      if (res.code !== 0) return;
      var list = (res.data && res.data.list) || [];
      var tbody = document.getElementById('log-body');
      if (!list.length) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty">暂无日志</td></tr>';
        return;
      }
      var html = '';
      list.forEach(function (l) {
        html += '<tr><td>' + l.time + '</td>' +
          '<td><span class="lv ' + l.level + '">' + l.level + '</span></td>' +
          '<td>' + l.module + '</td>' +
          '<td>' + l.message + '</td></tr>';
      });
      tbody.innerHTML = html;
    }).catch(function () { /* 静默失败，不打断看板 */ });
  }

  document.getElementById('btn-refresh-log').addEventListener('click', refreshLogs);

  /* ---------- 9. 启动 ---------- */
  loading.classList.add('show');
  ensureECharts(function (ok) {
    loading.classList.remove('show');
    if (ok) {
      initRealtime();
      initHistory();
    } else {
      emptyState(document.getElementById('chart-realtime'),
        '未加载到 ECharts 图表库：请连接网络，或把 echarts.min.js 放到 js/ 目录下');
      emptyState(document.getElementById('chart-history'), '同上');
    }

    refreshStatus();
    refreshLogs();
    tick();                                   // 立即拉一次
    startPoll(Number(document.getElementById('poll-select').value));

    queryHistory();                           // 默认查最近 24 小时
    setInterval(refreshStatus, 5000);
    setInterval(refreshLogs, 10000);

    window.addEventListener('resize', function () {
      if (charts.realtime) charts.realtime.resize();
      if (charts.history) charts.history.resize();
    });
  });
})();
