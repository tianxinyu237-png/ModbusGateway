#!/bin/bash
# ============================================================
# 会话持久化测试：登录拿到 token → 重启 web 服务 → 旧 token 依然可用 → 退出后失效
# 用法： bash tools/test_session_persist.sh   （web 服务需已在 8080 运行）
# ============================================================
P=$(cd "$(dirname "$0")/.." && pwd)
cd $P
PORT=8080
PASS=0; FAIL=0
ok(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

get_token() {
    python3 - <<'PY'
import json, urllib.request
r = urllib.request.Request("http://127.0.0.1:8080/api/login",
        data=json.dumps({"username":"admin","password":"123456"}).encode(),
        method="POST", headers={"Content-Type":"application/json"})
print(json.loads(urllib.request.urlopen(r, timeout=8).read())["data"]["token"])
PY
}
use_token() {   # $1=token  返回 HTTP 状态码
    python3 - "$1" <<'PY'
import sys, urllib.request, urllib.error
r = urllib.request.Request("http://127.0.0.1:8080/api/realtime")
r.add_header("Authorization", "Bearer " + sys.argv[1])
try:
    with urllib.request.urlopen(r, timeout=8) as x: print(x.status)
except urllib.error.HTTPError as e: print(e.code)
except Exception as e: print("EXC")
PY
}

echo "===== 1) 登录拿 token ====="
TOK=$(get_token)
[ ${#TOK} -ge 16 ] && ok "拿到 token（${TOK:0:12}…）" || bad "没拿到 token"
[ "$(use_token $TOK)" = "200" ] && ok "重启前 token 可用（200）" || bad "重启前就不可用"

echo "===== 2) 重启 web 服务（模拟你 make down / 换端口重启） ====="
pkill -f "thttpd.out $PORT" 2>/dev/null; sleep 1
nohup ./thttpd.out $PORT > logs/web.log 2>&1 &
sleep 1.5
pgrep -f "thttpd.out $PORT" >/dev/null && ok "web 服务已重启" || bad "web 服务没起来"

echo "===== 3) 用重启前的旧 token 再访问（会话存库，应该仍然有效） ====="
ST=$(use_token $TOK)
[ "$ST" = "200" ] && ok "重启后旧 token 依然可用（200）—— 不用重新登录" || bad "重启后旧 token 失效了（$ST）"

echo "===== 4) 退出登录后旧 token 立刻失效 ====="
python3 - "$TOK" <<'PY'
import sys, json, urllib.request
r = urllib.request.Request("http://127.0.0.1:8080/api/logout", data=b"{}", method="POST",
                           headers={"Content-Type":"application/json",
                                    "Authorization":"Bearer "+sys.argv[1]})
urllib.request.urlopen(r, timeout=8).read()
PY
ST=$(use_token $TOK)
[ "$ST" = "401" ] && ok "退出后旧 token 返回 401（会话已从库中删除）" || bad "退出后 token 还能用（$ST）"

echo
echo "===== 会话持久化测试汇总：PASS=$PASS  FAIL=$FAIL ====="
[ "$FAIL" -eq 0 ]
