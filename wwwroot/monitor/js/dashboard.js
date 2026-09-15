/* ============================================================================
   dashboard.js —— 数据看板逻辑
   对应需求文档：
     3.2 温湿度数据采集（轮询 /api/realtime，读的是共享内存实时值）
     3.3 数据展示（实时数值 + 实时曲线 + 历史记录查询）
     3.4 设备控制（指示灯按钮 → 消息队列 → 采集进程 → 置位从机寄存器）
     3.5 数据存储（历史记录表 / 日志记录表展示）
   ============================================================================ */

(function () {
  'use strict';

  var CFG = window.APP_CONFIG;

  /* ---------------- DOM ---------------- */
  var $ = function (id) { return document.getElementById(id); };
  var el = {
    banner: $('banner'), bannerText: $('banner-text'), bannerX: $('banner-x'),

    dotWeb: $('dot-web'), dotCollect: $('dot-collect'), dotModbus: $('dot-modbus'),
    dotSrc: $('dot-src'), pillSrc: $('pill-src'),
    clock: $('clock'),
    avatar: $('avatar'), uname: $('uname'), urole: $('urole'), btnLogout: $('btn-logout'),

    kpiTemp: $('kpi-temp'), kTemp: $('k-temp'), kTempFoot: $('k-temp-foot'),
    kpiHumi: $('kpi-humi'), kHumi: $('k-humi'), kHumiFoot: $('k-humi-foot'),
    kPeak: $('k-peak'), kCount: $('k-count'), kCountFoot: $('k-count-foot'),

    pollSelect: $('poll-select'), btnPause: $('btn-pause'),
    chart: $('chart-realtime'), chartTip: $('chart-tip'),

    lamp: $('lamp'), dState: $('d-state'), dMeta: $('d-meta'),
    ledSwitch: $('led-switch'), ledLabel: $('led-label'), btnLedRefresh: $('btn-led-refresh'),
    ledAddrTip: $('led-addr-tip'),

    paramPeriod: $('param-period'), btnPeriod: $('btn-period'),
    paramThreshold: $('param-threshold'), btnThreshold: $('btn-threshold'),
    regAddr: $('reg-addr'), regCount: $('reg-count'), btnReadReg: $('btn-readreg'), regOut: $('reg-out'),

    histHours: $('hist-hours'), btnHistQuery: $('btn-hist-query'), btnHistExport: $('btn-hist-export'),
    histTbody: $('hist-tbody'), histEmpty: $('hist-empty'),
    hsTavg: $('hs-tavg'), hsHavg: $('hs-havg'), hsCount: $('hs-count'),

    logTbody: $('log-tbody'), logEmpty: $('log-empty'),
    logFilter: $('log-filter'), btnLogRefresh: $('btn-log-refresh'),
    lsTotal: $('ls-total'), lsWarn: $('ls-warn'), lsErr: $('ls-err'),

    toasts: $('toasts'), apiBaseTip: $('api-base-tip'), footMode: $('foot-mode')
  };

  /* ---------------- 运行状态 ---------------- */
  var state = {
    timer: null,            /* 实时轮询定时器 */
    logTimer: null,
    running: false,         /* 是否在轮询 */
    interval: CFG.POLL_INTERVAL,
    chart: null,
    labels: [], temps: [], humis: [],
    ledKnown: null,         /* null=未知 0=灭 1=亮 */
    ledBusy: false,
    histList: [],
    online: true
  };

  /* ==========================================================================
     小工具
     ========================================================================== */
  function pad2(n) { return (n < 10 ? '0' : '') + n; }

  function toast(title, desc, type) {
    var d = document.createElement('div');
    d.className = 'toast ' + (type || '');
    d.innerHTML = '<b>' + title + '</b>' + (desc ? '<span>' + desc + '</span>' : '');
    el.toasts.appendChild(d);
    setTimeout(function () {
      d.classList.add('out');
      setTimeout(function () { d.remove(); }, 260);
    }, 3600);
  }

  function esc(s) {
    return String(s === null || s === undefined ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function n1(v) { return (v === null || v === undefined || isNaN(v)) ? '--' : Number(v).toFixed(1); }

  function setDot(dot, cls) { dot.className = 'dot ' + (cls || ''); }

  function showBanner(text, type) {
    el.bannerText.textContent = text;
    el.banner.className = 'banner show ' + (type || 'warn');
  }
  function hideBanner() { el.banner.className = 'banner'; }

  function busy(btn, on) {
    btn.disabled = on;
    btn.style.opacity = on ? '.6' : '';
  }

  /* ==========================================================================
     登录守卫：没有 token 直接回登录页
     ========================================================================== */
  var me = window.API.Session.get();
  if (!me || !window.API.Session.token()) {
    location.replace('index.html');
    return;
  }
  el.uname.textContent = me.username || '--';
  el.urole.textContent = me.role === 'admin' ? '管理员' : '普通用户';
  el.avatar.textContent = (me.username || 'U').slice(0, 1).toUpperCase();

  /* ==========================================================================
     时钟
     ========================================================================== */
  setInterval(function () {
    var d = new Date();
    el.clock.textContent = pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }, 1000);

  /* ==========================================================================
     ECharts 加载（CDN 失败自动回落本地 js/echarts.min.js）
     ========================================================================== */
  function ensureECharts(cb) {
    if (window.echarts) { cb(); return; }
    var cdns = [
      'https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js',
      'https://unpkg.com/echarts@5.5.0/dist/echarts.min.js'
    ];
    var i = 0;
    function local() {
      var s = document.createElement('script');
      s.src = 'js/echarts.min.js';
      s.onload = cb;
      s.onerror = function () {
        el.chart.innerHTML = '<div class="empty">图表库加载失败（CDN 与本地 js/echarts.min.js 都不可用）<br>' +
          '数值仍在实时刷新，只是没有曲线。</div>';
      };
      document.head.appendChild(s);
    }
    (function next() {
      if (i >= cdns.length) { local(); return; }
      var s = document.createElement('script');
      s.src = cdns[i++];
      s.onload = cb;
      s.onerror = next;
      document.head.appendChild(s);
    })();
  }

  function initChart() {
    if (!window.echarts || el.chart.clientWidth === 0) { if (window.echarts) el.chartTip.textContent = ''; return; }
    state.chart = window.echarts.init(el.chart, null, { renderer: 'canvas' });
    var axisLine = 'rgba(255,255,255,0.10)';
    var splitLine = 'rgba(255,255,255,0.055)';

    state.chart.setOption({
      animation: true,
      animationDuration: 260,
      grid: { left: 46, right: 48, top: 22, bottom: 26 },
      tooltip: {
        trigger: 'axis',
        backgroundColor: 'rgba(10,17,32,0.94)',
        borderColor: 'rgba(255,255,255,0.12)',
        textStyle: { color: '#e9f0fa', fontSize: 12 },
        axisPointer: { lineStyle: { color: 'rgba(34,211,238,0.5)' } }
      },
      xAxis: {
        type: 'category',
        boundaryGap: false,
        data: [],
        axisLine: { lineStyle: { color: axisLine } },
        axisLabel: { color: '#6d7f97', fontSize: 10, interval: Math.floor(CFG.REALTIME_POINTS / 6) },
        axisTick: { show: false }
      },
      yAxis: [
        {
          type: 'value', name: '℃', scale: true,
          nameTextStyle: { color: '#fdba74', fontSize: 10 },
          axisLine: { lineStyle: { color: axisLine } },
          axisLabel: { color: '#9aa9bd', fontSize: 10 },
          splitLine: { lineStyle: { color: splitLine } }
        },
        {
          type: 'value', name: '%RH', scale: true,
          nameTextStyle: { color: '#67e8f9', fontSize: 10 },
          axisLine: { lineStyle: { color: axisLine } },
          axisLabel: { color: '#9aa9bd', fontSize: 10 },
          splitLine: { show: false }
        }
      ],
      series: [
        {
          name: '温度', type: 'line', yAxisIndex: 0, smooth: true, showSymbol: false,
          lineStyle: { width: 2, color: '#fb923c' },
          itemStyle: { color: '#fb923c' },
          areaStyle: {
            color: new window.echarts.graphic.LinearGradient(0, 0, 0, 1, [
              { offset: 0, color: 'rgba(251,146,60,0.32)' },
              { offset: 1, color: 'rgba(251,146,60,0.01)' }
            ])
          },
          data: []
        },
        {
          name: '湿度', type: 'line', yAxisIndex: 1, smooth: true, showSymbol: false,
          lineStyle: { width: 2, color: '#22d3ee' },
          itemStyle: { color: '#22d3ee' },
          areaStyle: {
            color: new window.echarts.graphic.LinearGradient(0, 0, 0, 1, [
              { offset: 0, color: 'rgba(34,211,238,0.26)' },
              { offset: 1, color: 'rgba(34,211,238,0.01)' }
            ])
          },
          data: []
        }
      ]
    });

    window.addEventListener('resize', function () {
      if (state.chart) state.chart.resize();
    });
  }

  function pushChart(time, temp, humi) {
    state.labels.push(time);
    state.temps.push(temp);
    state.humis.push(humi);
    var max = CFG.REALTIME_POINTS;
    if (state.labels.length > max) {
      state.labels.splice(0, state.labels.length - max);
      state.temps.splice(0, state.temps.length - max);
      state.humis.splice(0, state.humis.length - max);
    }
    el.chartTip.textContent = '已累积 ' + state.labels.length + ' / ' + max + ' 个点（来自共享内存实时值）';
    if (state.chart) {
      state.chart.setOption({
        xAxis: { data: state.labels },
        series: [{ data: state.temps }, { data: state.humis }]
      });
    }
  }

  /* ==========================================================================
     指示灯
     ========================================================================== */
  function paintLed(on, known, meta) {
    state.ledKnown = known ? (on ? 1 : 0) : null;
    el.lamp.className = 'lamp ' + (known ? (on ? 'on' : 'off') : 'unknown');
    el.dState.className = 'd-state ' + (known ? (on ? 'on' : 'off') : '');
    el.dState.textContent = known
      ? (on ? '指示灯已点亮（寄存器置位）' : '指示灯已熄灭（寄存器复位）')
      : '指示灯状态未知';
    if (meta) el.dMeta.textContent = meta;
    el.ledSwitch.checked = known ? !!on : false;
    el.ledLabel.textContent = known ? (on ? '复位（熄灭）' : '置位（点亮）') : '置位 / 复位';
  }

  function setLed(on) {
    if (state.ledBusy) return;
    state.ledBusy = true;
    busy(el.btnLedRefresh, true);
    el.ledSwitch.disabled = true;

    window.API.command('setLed', { on: on }).then(function (data) {
      paintLed(on, true, '指令已下发 · 寄存器 0x' + String(data.addr || CFG.LED_REG).padStart(4, '0') +
        ' = ' + on + ' · ' + new Date().toLocaleTimeString());
      toast('指令下发成功', '指示灯' + (on ? '已置位（点亮）' : '已复位（熄灭）'), 'ok');
      loadLogs();
    }).catch(function (err) {
      /* 后端还没实现 setLed 时，明确告诉用户是后端缺接口，而不是"控件坏了" */
      var msg = err.message || '指令下发失败';
      if (err.code === 1004) {
        msg = '后端未实现 setLed 指令（见 README.md 第 3 节的后端待补清单）';
      }
      toast('指令下发失败', msg, 'err');
      paintLed(state.ledKnown === 1, state.ledKnown !== null, msg);
    }).then(function () {
      state.ledBusy = false;
      busy(el.btnLedRefresh, false);
      el.ledSwitch.disabled = false;
    });
  }

  el.ledSwitch.addEventListener('change', function () {
    setLed(el.ledSwitch.checked);
  });

  el.btnLedRefresh.addEventListener('click', function () {
    busy(el.btnLedRefresh, true);
    /* 优先用实时接口里的 led 字段；后端若没这个字段，就用读寄存器兜底 */
    window.API.realtime().then(function (d) {
      busy(el.btnLedRefresh, false);
      if (typeof d.led !== 'undefined') {
        paintLed(Number(d.led) === 1, true, '从实时数据回读 · ' + (d.collectTime || ''));
      } else {
        return window.API.command('readRegister', { addr: CFG.LED_REG, count: 1 }).then(function (r) {
          var v = r && r.regs && r.regs.length ? Number(r.regs[0]) : NaN;
          if (isNaN(v)) throw new Error('寄存器回读无数据');
          paintLed(v === 1, true, '从寄存器 0x' + String(CFG.LED_REG).padStart(4, '0') + ' 回读 = ' + v);
        });
      }
    }).catch(function (err) {
      busy(el.btnLedRefresh, false);
      paintLed(false, false, '回读失败：' + (err.message || '未知错误'));
      toast('回读失败', err.message || '未知错误', 'err');
    });
  });

  /* ==========================================================================
     采集参数下发
     ========================================================================== */
  el.btnPeriod.addEventListener('click', function () {
    var ms = parseInt(el.paramPeriod.value, 10);
    if (!(ms >= 100 && ms <= 60000)) { toast('参数非法', '采集周期需在 100 ~ 60000 ms 之间', 'err'); return; }
    busy(el.btnPeriod, true);
    window.API.command('setPeriod', { period: ms }).then(function (d) {
      toast('采集周期已下发', ms + ' ms（消息队列 CMD_SET_INTERVAL 已生效）', 'ok');
      state.interval = ms;
      el.kCountFoot.textContent = '采集周期 ' + ms + ' ms';
      loadLogs();
    }).catch(function (err) {
      toast('下发失败', err.message || '未知错误', 'err');
    }).then(function () { busy(el.btnPeriod, false); });
  });

  el.btnThreshold.addEventListener('click', function () {
    var v = Number(el.paramThreshold.value);
    if (!isFinite(v)) { toast('参数非法', '请输入合法的温度阈值', 'err'); return; }
    busy(el.btnThreshold, true);
    window.API.command('setThreshold', { threshold: v }).then(function () {
      toast('告警阈值已下发', v.toFixed(1) + ' ℃（消息队列 CMD_SET_THRESHOLD）', 'ok');
      loadLogs();
    }).catch(function (err) {
      toast('下发失败', err.message || '未知错误', 'err');
    }).then(function () { busy(el.btnThreshold, false); });
  });

  el.btnReadReg.addEventListener('click', function () {
    var addr = parseInt(el.regAddr.value, 10) || 0;
    var count = parseInt(el.regCount.value, 10) || 1;
    busy(el.btnReadReg, true);
    el.regOut.textContent = '读取中…';
    window.API.command('readRegister', { addr: addr, count: count }).then(function (d) {
      var regs = d.regs || [];
      el.regOut.textContent = '起始地址 ' + (d.regsAddr !== undefined ? d.regsAddr : addr) +
        '，共 ' + count + ' 个寄存器 → [' + regs.join(', ') + ']';
      toast('寄存器读取成功', count + ' 个寄存器已回读', 'ok');
      loadLogs();
    }).catch(function (err) {
      el.regOut.textContent = '读取失败：' + (err.message || '未知错误');
      toast('读取失败', err.message || '未知错误', 'err');
    }).then(function () { busy(el.btnReadReg, false); });
  });

  /* ==========================================================================
     实时数据轮询
     ========================================================================== */
  function renderRealtime(d) {
    /* 数值 */
    el.kTemp.innerHTML = n1(d.temp) + '<small>℃</small>';
    el.kHumi.innerHTML = n1(d.humi) + '<small>%RH</small>';
    el.kPeak.innerHTML = n1(d.tempMax) + ' / ' + n1(d.tempMin) + '<small>℃</small>';
    el.kCount.textContent = d.count != null ? d.count : '0';
    el.kCountFoot.textContent = '采集周期 ' + (d.period || '--') + ' ms';

    var th = Number(d.threshold || CFG.ALARM.tempMax);
    var alarm = Number(d.temp) >= th;
    el.kpiTemp.classList.toggle('alarm', alarm);
    el.kTempFoot.textContent = alarm
      ? '⚠ 超过告警阈值 ' + th.toFixed(1) + ' ℃'
      : '阈值 ' + th.toFixed(1) + ' ℃ · ' + (d.collectTime || '');
    el.kHumiFoot.textContent = 'slaveId ' + (d.slaveId != null ? d.slaveId : '--') +
      ' · ' + (d.online ? '从机在线' : '从机离线');

    el.paramThreshold.placeholder = th.toFixed(1);
    if (!el.paramPeriod.value) el.paramPeriod.placeholder = d.period || '2000';

    /* 曲线 */
    var t = (d.collectTime || '').slice(11) || new Date().toTimeString().slice(0, 8);
    pushChart(t, Number(d.temp), Number(d.humi));

    /* 指示灯（后端返回 led 字段时同步显示，不覆盖用户正在操作的开关） */
    if (typeof d.led !== 'undefined' && !state.ledBusy) {
      paintLed(Number(d.led) === 1, true,
        '从共享内存同步 · 寄存器 0x' + String(d.ledAddr || CFG.LED_REG).padStart(4, '0') +
        ' · ' + (d.collectTime || ''));
    }

    /* 数据来源标识 */
    if (d.source) {
      el.pillSrc.innerHTML = '';
      var dot = document.createElement('span');
      dot.className = 'dot ' + (d.source.simulated ? 'warn' : 'on');
      el.pillSrc.appendChild(dot);
      el.pillSrc.appendChild(document.createTextNode(
        '数据来源 ' + (d.source.name || '未知') + (d.source.simulated ? '（模拟设备）' : '（真实设备）')));
      el.pillSrc.title = d.source.target || '';
    }
    state.online = true;
  }

  function pollRealtime() {
    window.API.realtime().then(function (d) {
      renderRealtime(d);
      if (el.banner.classList.contains('show') && !el.banner.classList.contains('err')) hideBanner();
      setDot(el.dotCollect, 'on');
      setDot(el.dotModbus, d.online ? 'on' : 'off');
    }).catch(function (err) {
      state.online = false;
      setDot(el.dotCollect, 'off');
      setDot(el.dotModbus, 'off');
      el.kTempFoot.textContent = '采集数据获取失败';
      el.kHumiFoot.textContent = '采集数据获取失败';
      if (err.code === 2001) {
        showBanner('采集进程未运行（共享内存不存在）：请先启动 ./collector.out', 'err');
      } else if (err.code === 401) {
        /* 401 由 api.js 统一处理跳登录页 */
      } else {
        showBanner('实时数据获取失败：' + err.message, 'err');
      }
    });
  }

  function pollStatus() {
    window.API.status().then(function (s) {
      setDot(el.dotWeb, s.webOnline ? 'on' : 'off');
      setDot(el.dotCollect, s.collectOnline ? 'on' : 'off');
      setDot(el.dotModbus, s.modbusOnline ? 'on' : 'off');
      if (s.period) el.kCountFoot.textContent = '采集周期 ' + s.period + ' ms';
      if (typeof s.led !== 'undefined' && !state.ledBusy && state.ledKnown === null) {
        paintLed(Number(s.led) === 1, true, '从进程状态回读');
      }
      if (s.source === null && s.collectOnline === false) {
        showBanner('采集进程未运行：实时值为空，页面数值会停在最后一帧', 'err');
      }
    }).catch(function () {
      setDot(el.dotWeb, 'off');
    });
  }

  function startPolling() {
    stopPolling();
    state.running = true;
    el.btnPause.textContent = '暂停';
    el.btnPause.classList.add('on');
    pollRealtime();
    pollStatus();
    state.timer = setInterval(function () {
      pollRealtime();
      pollStatus();
    }, state.interval);
    state.logTimer = setInterval(loadLogs, 10000);
  }

  function stopPolling() {
    state.running = false;
    el.btnPause.textContent = '继续';
    el.btnPause.classList.remove('on');
    if (state.timer) { clearInterval(state.timer); state.timer = null; }
    if (state.logTimer) { clearInterval(state.logTimer); state.logTimer = null; }
  }

  el.btnPause.addEventListener('click', function () {
    if (state.running) { stopPolling(); toast('已暂停轮询', '点击「继续」恢复实时刷新', 'warn'); }
    else { startPolling(); toast('已恢复轮询', '每 ' + (state.interval / 1000) + ' 秒刷新一次', 'ok'); }
  });

  el.pollSelect.addEventListener('change', function () {
    state.interval = parseInt(el.pollSelect.value, 10) || CFG.POLL_INTERVAL;
    if (state.running) startPolling();
  });

  /* ==========================================================================
     历史记录
     ========================================================================== */
  function loadHistory() {
    var hours = parseInt(el.histHours.value, 10) || CFG.HISTORY_DEFAULT_HOURS;
    busy(el.btnHistQuery, true);
    el.histTbody.innerHTML = '<tr><td colspan="3" class="empty">查询中…</td></tr>';
    el.histEmpty.style.display = 'none';

    window.API.history(hours).then(function (d) {
      var list = (d && d.list) || [];
      state.histList = list;
      busy(el.btnHistQuery, false);

      if (!list.length) {
        el.histTbody.innerHTML = '';
        el.histEmpty.style.display = 'block';
        el.hsTavg.textContent = '--';
        el.hsHavg.textContent = '--';
        el.hsCount.textContent = '0';
        return;
      }

      el.histEmpty.style.display = 'none';

      /* 统计 */
      var st = 0, sh = 0;
      for (var k = 0; k < list.length; k++) { st += Number(list[k].temp) || 0; sh += Number(list[k].humi) || 0; }
      el.hsTavg.textContent = n1(st / list.length) + ' ℃';
      el.hsHavg.textContent = n1(sh / list.length) + ' %RH';
      el.hsCount.textContent = list.length;

      /* 倒序显示（最新的在最上面），最多画 300 行避免卡顿 */
      var show = list.slice(-300).reverse();
      var html = [];
      for (var i = 0; i < show.length; i++) {
        html.push('<tr><td>' + esc(show[i].time) + '</td>' +
          '<td class="num">' + n1(show[i].temp) + '</td>' +
          '<td class="num">' + n1(show[i].humi) + '</td></tr>');
      }
      el.histTbody.innerHTML = html.join('');
      toast('历史查询完成', '近 ' + hours + ' 小时共 ' + list.length + ' 条记录', 'ok');
    }).catch(function (err) {
      busy(el.btnHistQuery, false);
      el.histTbody.innerHTML = '';
      el.histEmpty.style.display = 'block';
      el.histEmpty.textContent = '查询失败：' + (err.message || '未知错误');
      toast('历史查询失败', err.message || '未知错误', 'err');
    });
  }

  el.btnHistQuery.addEventListener('click', loadHistory);

  el.btnHistExport.addEventListener('click', function () {
    if (!state.histList.length) { toast('没有可导出的数据', '请先查询历史记录', 'warn'); return; }
    var lines = ['采集时间,温度(℃),湿度(%RH)'];
    for (var i = 0; i < state.histList.length; i++) {
      var r = state.histList[i];
      lines.push(r.time + ',' + n1(r.temp) + ',' + n1(r.humi));
    }
    /* 加 BOM 让 Excel 正确识别 UTF-8 */
    var blob = new Blob(['\ufeff' + lines.join('\r\n')], { type: 'text/csv;charset=utf-8' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = '温湿度历史记录_' + new Date().toISOString().slice(0, 10) + '.csv';
    document.body.appendChild(a); a.click(); a.remove();
    toast('已导出 CSV', state.histList.length + ' 条记录', 'ok');
  });

  /* ==========================================================================
     日志
     ========================================================================== */
  function loadLogs() {
    window.API.logs().then(function (d) {
      var all = (d && d.list) || [];

      /* 统计条（按全部日志算，不受级别筛选影响） */
      var warn = 0, err = 0;
      for (var a = 0; a < all.length; a++) {
        if (all[a].level === 'WARN') warn++;
        else if (all[a].level === 'ERROR') err++;
      }
      el.lsTotal.textContent = all.length;
      el.lsWarn.textContent = warn;
      el.lsErr.textContent = err;

      var list = all;
      var f = el.logFilter.value;
      if (f) list = list.filter(function (x) { return x.level === f; });

      if (!list.length) {
        el.logTbody.innerHTML = '';
        el.logEmpty.style.display = 'block';
        el.logEmpty.textContent = f ? '没有 ' + f + ' 级别的日志' : '暂无日志';
        return;
      }
      el.logEmpty.style.display = 'none';
      var html = [];
      for (var i = 0; i < list.length; i++) {
        var r = list[i];
        html.push('<tr><td style="white-space:nowrap">' + esc((r.time || '').slice(5)) + '</td>' +
          '<td><span class="lv ' + esc(r.level) + '">' + esc(r.level) + '</span></td>' +
          '<td>' + esc(r.module) + '</td>' +
          '<td style="font-family:inherit;color:var(--txt-2)">' + esc(r.message) + '</td></tr>');
      }
      el.logTbody.innerHTML = html.join('');
    }).catch(function () {
      /* 日志接口失败不打扰用户，静默 */
    });
  }

  el.btnLogRefresh.addEventListener('click', loadLogs);
  el.logFilter.addEventListener('change', loadLogs);

  /* ==========================================================================
     退出 / 横幅关闭 / 降级提示
     ========================================================================== */
  el.btnLogout.addEventListener('click', function () {
    stopPolling();
    window.API.logout();
  });

  el.bannerX.addEventListener('click', hideBanner);

  window.addEventListener('api:fallback', function (e) {
    showBanner('未检测到后端服务（' + CFG.API_BASE + '），当前显示的是本地演示数据 —— ' +
      '后端起起来并刷新即可自动切回真实数据。', 'warn');
    el.footMode.textContent = '数据模式：本地演示数据';
  });

  /* ==========================================================================
     启动
     ========================================================================== */
  el.apiBaseTip.textContent = CFG.API_BASE;
  el.footMode.textContent = '数据模式：' + (window.API.mode() === 'mock' ? '本地演示数据' : '真实后端');
  if (window.API.mode() === 'mock') {
    showBanner('MOCK 模式：当前显示的是本地演示数据，不访问后端', 'warn');
  }

  paintLed(false, false, '等待从机回读…');
  ensureECharts(initChart);
  loadHistory();
  loadLogs();
  startPolling();

  /* 页面隐藏时停轮询，省资源（切回来自动恢复） */
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) stopPolling();
    else if (!state.running) startPolling();
  });
})();
