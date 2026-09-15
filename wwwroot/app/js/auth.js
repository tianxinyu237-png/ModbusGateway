/* ============================================================
   auth.js —— 登录 / 注册页逻辑
   链路：浏览器表单 → HTTP POST → webserver → 用户信息表
   ============================================================ */

(function () {
  var CFG = window.APP_CONFIG;
  var loading = document.getElementById('loading');

  function showLoading(on) {
    loading.classList.toggle('show', !!on);
  }

  function setTip(el, msg, type) {
    el.className = 'tip ' + (type || 'info');
    el.textContent = msg || '';
  }

  /* ---------- 已登录则直接进看板 ---------- */
  var user = API.Session.get();
  if (user && user.username) {
    location.replace('dashboard.html');
    return;
  }

  /* ---------- 切换 Tab ---------- */
  var tabs = document.querySelectorAll('.tab');
  var forms = {
    login: document.getElementById('login-form'),
    register: document.getElementById('register-form')
  };
  Array.prototype.forEach.call(tabs, function (tab) {
    tab.addEventListener('click', function () {
      var name = tab.getAttribute('data-tab');
      Array.prototype.forEach.call(tabs, function (t) {
        t.classList.toggle('active', t === tab);
      });
      Object.keys(forms).forEach(function (k) {
        forms[k].classList.toggle('active', k === name);
      });
    });
  });

  /* ---------- 登录 ---------- */
  var loginTip = document.getElementById('login-tip');
  var loginBtn = document.getElementById('login-btn');

  forms.login.addEventListener('submit', function (e) {
    e.preventDefault();
    var username = document.getElementById('login-user').value.trim();
    var password = document.getElementById('login-pass').value;

    if (!username || !password) {
      setTip(loginTip, '请输入用户名和密码', 'error');
      return;
    }

    loginBtn.disabled = true;
    setTip(loginTip, '正在登录…', 'info');
    showLoading(true);

    API.login(username, password).then(function (res) {
      if (res.code === 0) {
        API.Session.save(res.data);
        setTip(loginTip, '登录成功，正在进入数据看板…', 'ok');
        setTimeout(function () { location.href = 'dashboard.html'; }, 400);
      } else {
        setTip(loginTip, res.message || '登录失败', 'error');
        loginBtn.disabled = false;
      }
    }).catch(function (err) {
      setTip(loginTip, '请求失败：' + err.message + '（请确认 webserver 已启动）', 'error');
      loginBtn.disabled = false;
    }).then(function () {
      showLoading(false);
    });
  });

  /* ---------- 注册 ---------- */
  var regTip = document.getElementById('reg-tip');
  var regBtn = document.getElementById('reg-btn');

  forms.register.addEventListener('submit', function (e) {
    e.preventDefault();
    var username = document.getElementById('reg-user').value.trim();
    var password = document.getElementById('reg-pass').value;
    var password2 = document.getElementById('reg-pass2').value;

    if (!/^[A-Za-z0-9_]{4,16}$/.test(username)) {
      setTip(regTip, '用户名需为 4 ~ 16 位字母、数字或下划线', 'error');
      return;
    }
    if (password.length < 6) {
      setTip(regTip, '密码长度不能少于 6 位', 'error');
      return;
    }
    if (password !== password2) {
      setTip(regTip, '两次输入的密码不一致', 'error');
      return;
    }

    regBtn.disabled = true;
    setTip(regTip, '正在提交注册…', 'info');
    showLoading(true);

    API.register(username, password).then(function (res) {
      if (res.code === 0) {
        setTip(regTip, '注册成功，请使用新账号登录', 'ok');
        forms.register.reset();
        // 自动切回登录页并回填用户名
        setTimeout(function () {
          tabs[0].click();
          document.getElementById('login-user').value = username;
          document.getElementById('login-pass').focus();
          setTip(loginTip, '账号 ' + username + ' 注册成功，请输入密码登录', 'ok');
        }, 700);
      } else {
        setTip(regTip, res.message || '注册失败', 'error');
      }
    }).catch(function (err) {
      setTip(regTip, '请求失败：' + err.message, 'error');
    }).then(function () {
      regBtn.disabled = false;
      showLoading(false);
    });
  });

  /* ---------- 模拟模式提示 ---------- */
  if (CFG.MOCK) {
    setTip(loginTip, '当前为模拟数据模式（未连接 webserver），可直接用演示账号登录', 'info');
  }
  /* webserver 重启过 / token 失效被踢回来时，给个明确说法，别让用户以为坏了 */
  if (/[?&]expired=1/.test(location.search)) {
    setTip(loginTip, '登录状态已失效（webserver 重启会清空会话），请重新登录', 'error');
  }
})();
