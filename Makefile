# ==========================================================================
# ModbusGateway —— 温湿度采集监控平台
#   make            编译全部（thttpd.out 网页服务 / collector.out 采集进程）
#   make test       一键跑全部自动化测试
#   make daemon     后台守护方式启动采集进程（异常自动拉起）
#   make stop       停止采集守护进程
#   make status     查看采集进程状态
#   make init-db    用 db/init.sql 重建数据库（会清空历史数据！）
#   make clean      清理编译产物
# ==========================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -g -I.
LDFLAGS = -lpthread -lmodbus -lsqlite3 -lcjson -lrt

DB      = sensor.db
PORT    = 8080

# ---------- 源文件 ----------
WEB_SRC = main.c thttpd.c custom_handle.c api_rest.c \
          src/ipc/shm.c src/ipc/mq.c src/db/db.c
COL_SRC = src/collector/modbus_collector.c \
          src/ipc/shm.c src/ipc/mq.c src/db/db.c

# ---------- 编译 ----------
all: thttpd.out collector.out

# web服务器（手写HTTP解析 + REST接口 + 静态托管）
thttpd.out: $(WEB_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 采集进程（Modbus TCP → 共享内存 + SQLite，支持守护模式）
collector.out: $(COL_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---------- 运行 ----------
web: thttpd.out
	./thttpd.out $(PORT)

collector: collector.out
	./collector.out

daemon: collector.out
	./collector.out --daemon
	@sleep 1
	@./collector.out --status

stop: collector.out
	./collector.out --stop

status: collector.out
	./collector.out --status

# 一键起/停全部（从机模拟器 + 采集守护进程 + web，都在后台，不占终端）
up: all
	bash tools/start_all.sh $(PORT)

down:
	bash tools/stop_all.sh

# ---------- 测试 ----------
test: thttpd.out collector.out
	bash tools/run_all_tests.sh

# ---------- 数据库 ----------
init-db:
	python3 -c "import sqlite3;con=sqlite3.connect('$(DB)');con.executescript(open('db/init.sql',encoding='utf-8').read());con.commit();con.close();print('已用 db/init.sql 重建 $(DB)')"

# 清理测试数据（默认清测试用户+日志，保留历史曲线；备份自动生成）
clean-db:
	bash tools/clean_db.sh

db-stats:
	bash tools/clean_db.sh --stats

# ---------- 清理 ----------
clean:
	rm -f *.out
	rm -rf run logs tools/__pycache__
	rm -f selftest.db selftest.db-shm selftest.db-wal tools/.db_selftest.bin

# 连数据库和日志一起清（谨慎）
distclean: clean
	rm -f $(DB) $(DB)-shm $(DB)-wal

.PHONY: all web collector daemon stop status up down test init-db clean-db db-stats clean distclean
