/* ============================================================
   api.js —— 接口封装层
   统一出口：window.API.xxx()
   - MOCK = true  ：走 mock.js 本地模拟
   - MOCK = false ：走 fetch → 自研 webserver RESTful 接口

   后端接口约定（详见 README.md）：
     POST /api/register          { username, password }
     POST /api/login             { username, password }
     POST /api/logout            {}
     GET  /api/realtime          → 共享内存实时温湿度
     GET  /api/history?hours=24  → SQLite 历史记录
     POST /api/command           { action, ...params }  → 消息队列
     GET  /api/logs              → 日志记录表
     GET  /api/status            → 各进程运行状态
   ============================================================ */

(function () {
  var CFG = window.APP_CONFIG;
  var TOKEN_KEY = 'mg_token';
  var USER_KEY = 'mg_user';

  /* ---------- Session 管理 ---------- */
  var Session = {
    save: function (user) {
      sessionStorage.setItem(USER_KEY, JSON.stringify(user));
      sessionStorage.setItem(TOKEN_KEY, user.token || '');
      // 勾选"记住我"时同步到 localStorage
      localStorage.setItem(USER_KEY, JSON.stringify(user));
    },
    get: function () {
      try {
        var raw = sessionStorage.getItem(USER_KEY) || localStorage.getItem(USER_KEY);
        return raw ? JSON.parse(raw) : null;
      } catch (e) { return null; }
    },
    clear: function () {
      sessionStorage.removeItem(USER_KEY);
      sessionStorage.removeItem(TOKEN_KEY);
      localStorage.removeItem(USER_KEY);
    },
    token: function () {
      return sessionStorage.getItem(TOKEN_KEY) || '';
    }
  };

  /* ---------- 基础请求 ---------- */
  function request(path, options) {
    options = options || {};
    var url = CFG.API_BASE + path;
    var headers = { 'Content-Type': 'application/json' };
    var token = Session.token();
    if (token) headers['Authorization'] = 'Bearer ' + token;

    var cfg = {
      method: options.method || 'GET',
      headers: headers
    };
    if (options.body) cfg.body = JSON.stringify(options.body);

    return fetch(url, cfg).then(function (res) {
      if (res.status === 401) {
        // token 失效（会话存在数据库 sessions 表里，TTL 2 小时；过期或退出登录后就会 401）
        // 清掉本地登录态并回登录页，带上 expired 标记让登录页给出提示
        Session.clear();
        location.href = 'index.html?expired=1';
        throw new Error('未登录或登录已过期');
      }
      if (!res.ok) throw new Error('HTTP ' + res.status + ' ' + res.statusText);
      return res.json();
    });
  }

  /* 模拟模式下的延迟，让加载态可见 */
  function mockCall(fn) {
    return new Promise(function (resolve) {
      setTimeout(function () { resolve(fn()); }, 160);
    });
  }

  /* ---------- 统一出口 ---------- */
  window.API = {
    Session: Session,

    register: function (username, password) {
      if (CFG.MOCK) return mockCall(function () { return window.Mock.register(username, password); });
      return request('/register', { method: 'POST', body: { username: username, password: password } });
    },

    login: function (username, password) {
      if (CFG.MOCK) return mockCall(function () { return window.Mock.login(username, password); });
      return request('/login', { method: 'POST', body: { username: username, password: password } });
    },

    logout: function () {
      var done = function () { Session.clear(); location.href = 'index.html'; };
      if (CFG.MOCK) { done(); return Promise.resolve(); }
      return request('/logout', { method: 'POST' }).then(done, done);
    },

    /* 实时数据：webserver 读共享内存后返回 */
    realtime: function () {
      if (CFG.MOCK) return mockCall(function () { return window.Mock.realtime(); });
      return request('/realtime');
    },

    /* 历史数据：来自 SQLite 温湿度采集记录表 */
    history: function (hours) {
      if (CFG.MOCK) return mockCall(function () { return window.Mock.history(hours); });
      return request('/history?hours=' + (hours || 24));
    },

    /* 指令下发：webserver 投递到 POSIX 消息队列 */
    command: function (action, params) {
      params = params || {};
      var body = Object.assign({ action: action }, params);
      if (CFG.MOCK) return mockCall(function () { return window.Mock.command(action, params); });
      return request('/command', { method: 'POST', body: body });
    },

    logs: function () {
      if (CFG.MOCK) return mockCall(function () { return window.Mock.logs(); });
      return request('/logs');
    },

    status: function () {
      if (CFG.MOCK) return mockCall(function () { return window.Mock.status(); });
      return request('/status');
    }
  };
})();
