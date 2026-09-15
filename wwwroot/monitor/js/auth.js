/* ============================================================================
   auth.js —— 登录 / 注册页逻辑
   对应需求文档 3.1 用户管理模块：
     · 注册：账号唯一性 + 密码格式校验 → 通过后写入用户信息表
     · 登录：账号密码校验 → 通过后跳转数据展示页面；失败给出明确提示
   ============================================================================ */

(function () {
  'use strict';

  var CFG = window.APP_CONFIG;

  var el = {
    title:      document.getElementById('card-title'),
    hint:       document.getElementById('card-hint'),
    tabLogin:   document.getElementById('tab-login'),
    tabReg:     document.getElementById('tab-reg'),
    msg:        document.getElementById('msg'),
    form:       document.getElementById('form'),
    username:   document.getElementById('username'),
    password:   document.getElementById('password'),
    confirm:    document.getElementById('confirm'),
    fieldConf:  document.getElementById('field-confirm'),
    eye:        document.getElementById('eye'),
    remember:   document.getElementById('remember'),
    submit:     document.getElementById('submit'),
    submitText: document.getElementById('submit-text'),
    strength:   document.getElementById('strength'),
    modeTip:    document.getElementById('mode-tip')
  };

  var mode = 'login';   /* 当前模式：login | register */

  /* ---------------- 工具 ---------------- */
  function showMsg(text, type) {
    el.msg.className = 'msg show ' + (type || 'info');
    el.msg.textContent = text;
  }
  function hideMsg() { el.msg.className = 'msg'; }

  function setErr(inputId, errId, text) {
    var input = document.getElementById(inputId);
    var box = document.getElementById(errId);
    if (text) {
      input.classList.add('bad');
      box.textContent = text;
      box.classList.add('show');
    } else {
      input.classList.remove('bad');
      box.textContent = '';
      box.classList.remove('show');
    }
  }

  function setLoading(on, text) {
    el.submit.disabled = on;
    el.submit.classList.toggle('loading', on);
    el.submitText.textContent = on ? text : (mode === 'login' ? '登 录' : '注 册');
  }

  /* ---------------- 前端校验（后端仍会再校验一次） ---------------- */
  function checkUsername() {
    var v = el.username.value.trim();
    if (!v) { setErr('username', 'err-username', '请输入账号'); return false; }
    if (v.length < 3 || v.length > 32) { setErr('username', 'err-username', '账号长度需在 3~32 位之间'); return false; }
    if (!/^[A-Za-z0-9_.\u4e00-\u9fa5-]+$/.test(v)) {
      setErr('username', 'err-username', '账号只能包含字母、数字、下划线、点、连字符或中文');
      return false;
    }
    setErr('username', 'err-username', '');
    return true;
  }

  function checkPassword() {
    var v = el.password.value;
    if (!v) { setErr('password', 'err-password', '请输入密码'); return false; }
    if (v.length < 6) { setErr('password', 'err-password', '密码长度至少 6 位'); return false; }
    setErr('password', 'err-password', '');
    return true;
  }

  function checkConfirm() {
    if (mode !== 'register') return true;
    if (!el.confirm.value) { setErr('confirm', 'err-confirm', '请再次输入密码'); return false; }
    if (el.confirm.value !== el.password.value) { setErr('confirm', 'err-confirm', '两次输入的密码不一致'); return false; }
    setErr('confirm', 'err-confirm', '');
    return true;
  }

  /* 密码强度（注册时的提示条） */
  function strengthOf(p) {
    var s = 0;
    if (p.length >= 6) s++;
    if (p.length >= 10) s++;
    if (/[A-Za-z]/.test(p) && /[0-9]/.test(p)) s++;
    if (/[^A-Za-z0-9]/.test(p)) s++;
    return s;
  }
  function paintStrength() {
    if (mode !== 'register') { el.strength.style.display = 'none'; return; }
    var p = el.password.value;
    if (!p) { el.strength.style.display = 'none'; return; }
    el.strength.style.display = 'flex';
    el.strength.className = 'strength lv' + Math.max(1, strengthOf(p));
  }

  /* ---------------- 模式切换 ---------------- */
  function setMode(next) {
    mode = next;
    var isLogin = next === 'login';
    el.tabLogin.classList.toggle('on', isLogin);
    el.tabReg.classList.toggle('on', !isLogin);
    el.fieldConf.style.display = isLogin ? 'none' : 'block';
    el.title.textContent = isLogin ? '欢迎回来' : '创建新账号';
    el.hint.textContent = isLogin
      ? '请登录后进入温湿度数据看板'
      : '注册成功后即可登录查看实时数据';
    el.submitText.textContent = isLogin ? '登 录' : '注 册';
    el.password.setAttribute('autocomplete', isLogin ? 'current-password' : 'new-password');
    hideMsg();
    setErr('username', 'err-username', '');
    setErr('password', 'err-password', '');
    setErr('confirm', 'err-confirm', '');
    paintStrength();
    el.username.focus();
  }

  /* ---------------- 提交 ---------------- */
  el.form.addEventListener('submit', function (ev) {
    ev.preventDefault();
    hideMsg();

    if (!checkUsername() || !checkPassword() || !checkConfirm()) {
      showMsg('表单填写有误，请检查标红的输入项', 'err');
      return;
    }

    var username = el.username.value.trim();
    var password = el.password.value;
    var remember = el.remember.checked;

    if (mode === 'login') {
      setLoading(true, '登录中…');
      window.API.login(username, password).then(function (data) {
        window.API.Session.save({
          username: data.username || username,
          role: data.role || 'user',
          uid: data.uid,
          token: data.token
        }, remember);
        setLoading(true, '进入看板…');
        showMsg('登录成功，正在进入数据看板…', 'ok');
        setTimeout(function () { location.href = 'dashboard.html'; }, 350);
      }).catch(function (err) {
        setLoading(false);
        showMsg(err.message || '登录失败，请稍后重试', 'err');
        if (err.code === 1001) {
          setErr('password', 'err-password', '账号或密码错误');
        }
      });
    } else {
      setLoading(true, '注册中…');
      window.API.register(username, password).then(function () {
        setLoading(false);
        setMode('login');                 /* 切回登录 tab（会清掉提示） */
        el.username.value = username;
        el.password.value = '';
        el.confirm.value = '';
        showMsg('注册成功！已写入用户信息表，请使用该账号登录。', 'ok');
        el.password.focus();
      }).catch(function (err) {
        setLoading(false);
        showMsg(err.message || '注册失败，请稍后重试', 'err');
        if (err.code === 1002) setErr('username', 'err-username', '该账号已存在');
      });
    }
  });

  /* ---------------- 其它交互 ---------------- */
  el.tabLogin.addEventListener('click', function () { setMode('login'); });
  el.tabReg.addEventListener('click', function () { setMode('register'); });

  el.eye.addEventListener('click', function () {
    var show = el.password.type === 'password';
    el.password.type = show ? 'text' : 'password';
    el.eye.textContent = show ? '🙈' : '👁';
  });

  el.password.addEventListener('input', function () { paintStrength(); checkPassword(); });
  el.username.addEventListener('input', checkUsername);
  el.confirm.addEventListener('input', checkConfirm);

  /* 已登录过就直接进看板（除非是 token 过期被踢回来的） */
  (function gate() {
    var params = new URLSearchParams(location.search);
    if (params.get('expired')) {
      showMsg('登录状态已过期（会话 TTL 7200 秒），请重新登录', 'info');
      window.API.Session.clear();
      return;
    }
    if (window.API.Session.get() && window.API.Session.token()) {
      location.replace('dashboard.html');
      return;
    }
    el.username.focus();
  })();

  /* 后端连不上 → api.js 广播 api:fallback，这里提示用户当前是演示数据 */
  window.addEventListener('api:fallback', function () {
    showMsg('未检测到后端服务，已切换到本地演示数据（admin / 123456 可直接登录）', 'info');
    el.modeTip.textContent = '演示数据模式';
  });
  if (window.API.mode() === 'mock') { el.modeTip.textContent = '演示数据模式'; }

  document.getElementById('api-base-tip').textContent = CFG.API_BASE;
})();
