#ifndef DB_H
#define DB_H
#include <sqlite3.h>

/* ============ 数据库打开 / 建表 / 关闭 ============ */
int db_open(const char *db_path, sqlite3 **pdb);
// 执行初始化脚本 init.sql（建 users / sensor_data / logs 表，并设置WAL等PRAGMA）
int db_run_init_sql(sqlite3 *db, const char *sql_file);
// 运行期补建表：CREATE TABLE IF NOT EXISTS，不会删数据，程序启动时调一次即可
int db_init_runtime(sqlite3 *db);
void db_close(sqlite3 *db);

/* ============ 用户系统（密码加盐哈希，绝不存明文） ============ */
// 计算 salt + password 的 SHA-256，输出64个十六进制字符
int db_make_hash(const char *password, const char *salt, char *hex_out, int hex_len);
// 注册：内部生成随机盐并哈希，库里存 "salt$hash"
int db_user_register(sqlite3 *db, const char *username, const char *password);
// 登录校验：按用户名取出盐，用同样规则算哈希比对，成功返回0并把uid带出来
int db_user_login(sqlite3 *db, const char *username, const char *password, int *uid);

/* ============ 采集数据 ============ */
// 插入一条传感器记录，给collector采集进程调用
int db_insert_sensor_record(sqlite3 *db, float temp, float humi, int status);
// 查询最近N条，输出cJSON数组（字段 temp/humi/status/time）
int db_query_sensor_history(sqlite3 *db, char *out_json, int out_len, int limit);
// 按时间跨度查（最近 hours 小时），输出cJSON数组（字段 time/temp/humi，给网页图表）
int db_query_history_hours(sqlite3 *db, char *out_json, int out_len, int hours, int limit);
// 统计 sensor_data 记录条数
int db_count_sensor_records(sqlite3 *db, int *count);

/* ============ 登录会话 ============ */
// 会话存数据库（不是存进程内存）：webserver 重启后 token 依然有效，
// 而且多个 web 实例（比如同时开 80 和 8080）能互相认账
int db_session_create(sqlite3 *db, const char *token, const char *username,
                      const char *role, int ttl_sec);
// 校验会话：有效返回0并把用户名/角色带出来，无效/过期返回-1
int db_session_check(sqlite3 *db, const char *token, char *user_out, int user_len,
                     char *role_out, int role_len);
int db_session_delete(sqlite3 *db, const char *token);
int db_session_cleanup(sqlite3 *db);

/* ============ 运行日志 ============ */
// 写一条分级日志（DEBUG/INFO/WARN/ERROR），用法同 printf
int db_log(sqlite3 *db, const char *level, const char *module, const char *fmt, ...);
// 查询最近N条日志，输出cJSON数组（字段 time/level/module/message）
int db_query_logs(sqlite3 *db, char *out_json, int out_len, int limit);

#endif
