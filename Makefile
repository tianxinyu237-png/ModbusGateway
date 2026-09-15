# ==========================================================================
# ModbusGateway —— 工业物联网温湿度采集监控平台
#
#   目录约定：
#     src/       C 源码（http 层 / ipc / db / collector）
#     tests/     自动化测试（121 项断言，入口 tests/run_all_tests.sh）
#     tools/     运行与调试工具（从机模拟器、一键启停、数据库清理）
#     docs/      接口契约与开发手册
#     db/        建表脚本        wwwroot/  静态资源根（app/ 是前端）
#
#   常用命令：
#     make            编译 thttpd.out（web 服务） + collector.out（采集进程）
#     make up         一键后台起全部服务（从机模拟器 + 采集守护 + web）
#     make down       一键停全部服务
#     make test       跑全部自动化测试（121 项）
#     make status     看采集进程状态
#     make clean      清编译产物与运行日志
#     make init-db    用 db/init.sql 重建数据库（会清空历史数据！）
# ==========================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -g -I. -Isrc
LDFLAGS = -lpthread -lmodbus -lsqlite3 -lcjson -lrt

DB      = sensor.db
PORT    = 8080

# ---------- 源文件 ----------
HTTP_SRC = src/http/thttpd.c src/http/custom_handle.c src/http/api_rest.c
IPC_SRC  = src/ipc/shm.c src/ipc/mq.c
DB_SRC   = src/db/db.c

WEB_SRC = src/main.c $(HTTP_SRC) $(IPC_SRC) $(DB_SRC)
COL_SRC = src/collector/modbus_collector.c $(IPC_SRC) $(DB_SRC)

# ---------- 编译 ----------
all: thttpd.out collector.out

# web 服务器（手写HTTP解析 + REST接口 + 静态托管）
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

# 一键起/停全部（从机模拟器 + 采集守护 + web，都在后台，不占终端）
up: all
	bash tools/start_all.sh $(PORT)

down:
	bash tools/stop_all.sh

# ---------- 测试 ----------
test: thttpd.out collector.out
	bash tests/run_all_tests.sh

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
	rm -rf run logs tests/__pycache__ tools/__pycache__
	rm -f selftest.db selftest.db-shm selftest.db-wal tests/.db_selftest.bin

# 连数据库和日志一起清（谨慎）
distclean: clean
	rm -f $(DB) $(DB)-shm $(DB)-wal

.PHONY: all web collector daemon stop status up down test init-db clean-db db-stats clean distclean
