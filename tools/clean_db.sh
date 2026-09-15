#!/bin/bash
# ============================================================
# 数据库清理（答辩/演示前用，把测试期间产生的脏数据清掉）
#   用法:
#     bash tools/clean_db.sh              默认：删掉除 admin 外的所有测试用户 + 清掉测试日志
#     bash tools/clean_db.sh --users      只清测试用户（除 admin）
#     bash tools/clean_db.sh --logs       只清日志
#     bash tools/clean_db.sh --history    清空采集历史（图表会从零开始长）
#     bash tools/clean_db.sh --all        用户+日志+历史 全清（保留 admin 账号）
#     bash tools/clean_db.sh --stats      只看各表行数，不动数据
#   注意：清理前会自动备份成 sensor.db.bak_时间戳
# ============================================================
P=$(cd "$(dirname "$0")/.." && pwd)
cd $P
DB=sensor.db

FO_USERS=0; FO_LOGS=0; FO_HIST=0; FO_STATS=0
for a in "$@"; do
    case "$a" in
        --users)   FO_USERS=1 ;;
        --logs)    FO_LOGS=1 ;;
        --history) FO_HIST=1 ;;
        --all)     FO_USERS=1; FO_LOGS=1; FO_HIST=1 ;;
        --stats)   FO_STATS=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "未知参数: $a（用 --help 看用法）"; exit 2 ;;
    esac
done
if [ $FO_STATS -eq 0 ] && [ $FO_USERS -eq 0 ] && [ $FO_LOGS -eq 0 ] && [ $FO_HIST -eq 0 ]; then
    FO_USERS=1; FO_LOGS=1          # 默认：清测试用户 + 测试日志，保留历史曲线
fi

if [ ! -f $DB ]; then echo "没找到 $DB"; exit 1; fi

python3 - "$DB" "$FO_USERS" "$FO_LOGS" "$FO_HIST" "$FO_STATS" <<'PY'
import sqlite3, sys, os, shutil, time
db = sys.argv[1]
fu, fl, fh, fs = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])

def tables(con):
    return [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")]

con = sqlite3.connect(db)
have = tables(con)
print("===== 清理前 =====")
for t in ["sensor_data","users","logs"]:
    if t in have:
        print("  %-12s %d 行" % (t, con.execute("SELECT COUNT(*) FROM %s" % t).fetchone()[0]))

if fs:
    print("\n(只看统计，未改动)")
    if "users" in have:
        print("  用户列表:")
        for r in con.execute("SELECT id,username FROM users ORDER BY id"):
            print("    ", r)
    con.close(); sys.exit(0)

bak = "%s.bak_%s" % (db, time.strftime("%Y%m%d_%H%M%S"))
shutil.copyfile(db, bak)
print("\n已备份: %s" % bak)

if fu and "users" in have:
    n = con.execute("DELETE FROM users WHERE username <> 'admin'").rowcount
    print("  删除测试用户 %d 个（保留 admin）" % n)
    # admin 不存在就补一个，保证演示账号可用
    if con.execute("SELECT COUNT(*) FROM users WHERE username='admin'").fetchone()[0] == 0:
        print("  admin 不存在，跳过（请用网页注册页创建）")
if fl and "logs" in have:
    n = con.execute("DELETE FROM logs").rowcount
    print("  清空日志 %d 条" % n)
    con.execute("DELETE FROM sqlite_sequence WHERE name='logs'")
if fh and "sensor_data" in have:
    n = con.execute("DELETE FROM sensor_data").rowcount
    print("  清空采集历史 %d 条" % n)
    con.execute("DELETE FROM sqlite_sequence WHERE name='sensor_data'")
con.commit()
con.execute("VACUUM")          # 收缩文件体积
con.commit()

print("\n===== 清理后 =====")
for t in ["sensor_data","users","logs"]:
    if t in have:
        print("  %-12s %d 行" % (t, con.execute("SELECT COUNT(*) FROM %s" % t).fetchone()[0]))
if "users" in have:
    print("  当前用户:", [r[0] for r in con.execute("SELECT username FROM users ORDER BY id")])
con.close()
print("\n提示：采集进程正在运行时也能清（WAL模式），但图表要等新数据进来才有点。")
PY
