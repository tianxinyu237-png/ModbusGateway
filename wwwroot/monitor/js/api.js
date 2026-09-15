/* ============================================================================
   api.js —— 接口封装层
   统一出口：window.API.xxx()   页面里不要直接写 fetch

   后端接口契约（与 docs/API.md 一一对应）：
     POST /api/register   {username,password}        → data{uid,username}
     POST /api/login      {username,password}        → data{username,role,uid,token}
     POST /api/logout     {}                          → 旧 token 立即失效
     GET  /api/realtime                               → data{temp,humi,tempMax,tempMin,count,
                                                             slaveId,online,collectTime,period,
                                                             threshold,source{name,simulated,target},led}
     GET  /api/history?hours=24                       → data{hours,list:[{time,temp,humi}]}
     POST /api/command    {action,...}                → data{...}
     GET  /api/logs                                   → data{list:[{time,level,module,message}]}
     GET  /api/status                                 → data{collectOnline,webOnline,modbusOnline,
                                                             period,uptime,deviceStatus,slaveId,source}

   统一响应体 {code,message,data}；除注册/登录外都要带 Authorization: Bearer <token>；
   token 失效 → HTTP 401，本层负责清登录态并跳回登录页。
   ============================================================================ */

(function () {
  'use strict';

  var CFG = window.APP_CONFIG;
  var TOKEN_KEY = 'hms_token';
  var USER_KEY = 'hms_user';

  /* ------------------------------------------------------------------
     MOCK 模式状态机：real / mock / auto(未定)
     ------------------------------------------------------------------ */
  var MODE = CFG.MOCK === true ? 'mock' : (CFG.MOCK === false ? 'real' : 'auto');
  var fallbackFired = false;

  function isMock() { return MODE === 'mock'; }
  function isAuto() { return CFG.MOCK === 'auto'; }

  function degradeToMock(reason) {
    if (MODE !== 'auto') return;
    MODE = 'mock';
    if (!fallbackFired) {
      fallbackFired = true;
      window.dispatchEvent(new CustomEvent('api:fallback', {
        detail: { reason: reason || '后端不可达' }
      }));
    }
  }

  /* ------------------------------------------------------------------
     登录态（token + 用户信息）
     ------------------------------------------------------------------ */
  var Session = {
    save: function (user, remember) {
      var raw = JSON.stringify(user || {});
      var store = (remember || CFG.REMEMBER_DEFAULT) ? localStorage : sessionStorage;
      try {
        sessionStorage.removeItem(USER_KEY); sessionStorage.removeItem(TOKEN_KEY);
        localStorage.removeItem(USER_KEY); localStorage.removeItem(TOKEN_KEY);
        store.setItem(USER_KEY, raw);
        store.setItem(TOKEN_KEY, (user && user.token) || '');
        store.setItem('hms_remember', remember ? '1' : '0');
      } catch (e) { /* 隐私模式忽略 */ }
    },
    get: function () {
      var raw = null;
      try {
        raw = sessionStorage.getItem(USER_KEY) || localStorage.getItem(USER_KEY);
      } catch (e) { return null; }
      try { return raw ? JSON.parse(raw) : null; } catch (e) { return null; }
    },
    token: function () {
      try {
        return sessionStorage.getItem(TOKEN_KEY) || localStorage.getItem(TOKEN_KEY) || '';
      } catch (e) { return ''; }
    },
    clear: function () {
      try {
        sessionStorage.removeItem(USER_KEY); sessionStorage.removeItem(TOKEN_KEY);
        localStorage.removeItem(USER_KEY); localStorage.removeItem(TOKEN_KEY);
      } catch (e) { /* ignore */ }
    }
  };

  /* ------------------------------------------------------------------
     底层请求：只负责发请求 + 解统一响应体
     ------------------------------------------------------------------ */
  function request(path, options) {
    options = options || {};
    var cfg = {
      method: options.method || 'GET',
      headers: { 'Content-Type': 'application/json' },
      cache: 'no-store'
    };
    var token = Session.token();
    if (token) cfg.headers['Authorization'] = 'Bearer ' + token;
    if (options.body) cfg.body = JSON.stringify(options.body);

    return fetch(CFG.API_BASE + path, cfg).then(function (res) {
      /* token 缺失 / 失效 / 过期 → 清登录态回登录页 */
      if (res.status === 401) {
        Session.clear();
        if (!/index\.html/.test(location.pathname)) {
          location.replace('index.html?expired=1');
        }
        var e401 = new Error('登录已过期，请重新登录');
        e401.code = 401;
        throw e401;
      }
      return res.json().catch(function () {
        var e = new Error('后端响应不是合法 JSON（HTTP ' + res.status + '）');
        e.code = res.status;
        e.transport = true;
        throw e;
      });
    }).catch(function (err) {
      /* fetch 本身失败（后端没起 / 跨域被拒 / 网线断了）→ 标记 transport，
         交给上层 call() 决定是降级 mock 还是抛给调用方 */
      if (err && err.name === 'TypeError') {
        var e = new Error('无法连接后端（请求 ' + CFG.API_BASE + path + ' 失败）');
        e.transport = true;
        e.cause = err;
        throw e;
      }
      throw err;
    });
  }

  /* 解包 {code,message,data}；code!==0 一律 reject，带有后端给的中文 message */
  function unwrap(res) {
    if (!res || typeof res.code === 'undefined') {
      var e = new Error('后端响应缺少 code 字段');
      e.transport = true;
      throw e;
    }
    if (res.code !== 0) {
      var err = new Error(res.message || ('接口返回错误码 ' + res.code));
      err.code = res.code;
      throw err;
    }
    return res.data === undefined ? {} : res.data;
  }

  /* 模拟模式的假延迟，让加载态可见（不然瞬间返回看不到 loading） */
  function delayed(fn) {
    return new Promise(function (resolve, reject) {
      setTimeout(function () {
        try { resolve(fn()); } catch (e) { reject(e); }
      }, 140);
    });
  }

  /* 关键：real → mock 的统一调度
     - mock 模式      ：直接跑 mock
     - real 模式      ：只跑 real，出错就抛
     - auto 模式      ：先 real；网络层失败（transport）→ 降级 mock 并继续返回数据，
                        这样页面永远有东西看；业务错误（如 401 / 1001）照常抛，不掩盖问题 */
  function call(realFn, mockFn) {
    if (isMock()) return delayed(mockFn);
    return realFn().then(unwrap).catch(function (err) {
      if (isAuto() && err && err.transport) {
        degradeToMock(err.message);
        return delayed(mockFn);
      }
      throw err;
    });
  }

  /* ------------------------------------------------------------------
     统一出口
     ------------------------------------------------------------------ */
  window.API = {
    Session: Session,

    /* 当前实际数据来源，页面用来显示横幅：'real' | 'mock' */
    mode: function () { return isMock() ? 'mock' : 'real'; },

    register: function (username, password) {
      return call(
        function () { return request('/register', { method: 'POST', body: { username: username, password: password } }); },
        function () { return window.Mock.register(username, password); }
      );
    },

    login: function (username, password) {
      return call(
        function () { return request('/login', { method: 'POST', body: { username: username, password: password } }); },
        function () { return window.Mock.login(username, password); }
      );
    },

    /* 退出：不管后端成功失败，本地登录态都要清掉 */
    logout: function () {
      var finish = function () { Session.clear(); location.replace('index.html'); };
      if (isMock()) { finish(); return Promise.resolve(); }
      return request('/logout', { method: 'POST' })
        .catch(function () { /* 后端没起也能退出 */ })
        .then(finish);
    },

    /* 实时温湿度（webserver 读共享内存） */
    realtime: function () {
      return call(
        function () { return request('/realtime'); },
        function () { return window.Mock.realtime(); }
      );
    },

    /* 历史记录（SQLite 温湿度采集记录表） */
    history: function (hours) {
      hours = hours || CFG.HISTORY_DEFAULT_HOURS;
      return call(
        function () { return request('/history?hours=' + hours); },
        function () { return window.Mock.history(hours); }
      );
    },

    /* 指令下发（webserver 投递到 POSIX 消息队列 → 采集进程 → Modbus 从机）
       action: setPeriod | setThreshold | readRegister | restart | setLed */
    command: function (action, params) {
      var body = Object.assign({ action: action }, params || {});
      return call(
        function () { return request('/command', { method: 'POST', body: body }); },
        function () { return window.Mock.command(action, params); }
      );
    },

    /* 日志记录表 */
    logs: function () {
      return call(
        function () { return request('/logs'); },
        function () { return window.Mock.logs(); }
      );
    },

    /* 各进程运行状态 */
    status: function () {
      return call(
        function () { return request('/status'); },
        function () { return window.Mock.status(); }
      );
    }
  };
})();
