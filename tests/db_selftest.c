// db.c 功能自测：init.sql建表 / SHA-256 / 注册 / 登录 / 插记录 / 按跨度查历史 / 日志
// 编译： gcc -Wall -g -I. -Isrc -o tests/.db_selftest.bin tests/db_selftest.c src/db/db.c -lsqlite3 -lcjson
// （在工程根目录执行；tests/run_all_tests.sh 会自动做这一步）
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "db/db.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do{ \
    if(cond){ printf("  [PASS] %s\n", msg); pass++; } \
    else    { printf("  [FAIL] %s\n", msg); fail++; } \
}while(0)

int main(int argc, char *argv[])
{
    sqlite3 *db = NULL;
    const char *tmpdb = "./selftest.db";
    char hex[80];
    unlink(tmpdb);   // 每次都从干净的库开始

    printf("===== 0. SHA-256 正确性（对照标准测试向量） =====\n");
    // sha256("abc") 的官方向量：ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    // 我们的接口是 hash(salt + password)，所以 salt 传空串时结果就等于 sha256("abc")
    db_make_hash("abc", "", hex, sizeof(hex));
    printf("        sha256(\"abc\") = %s\n", hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256(\"abc\") 与标准向量一致");
    db_make_hash("", "", hex, sizeof(hex));
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "sha256(\"\") 与标准向量一致");
    // 加盐后必须和空盐不同
    char hex2[80];
    db_make_hash("abc", "salt123", hex2, sizeof(hex2));
    CHECK(strcmp(hex, hex2) != 0, "加盐后的哈希与不加盐不同");

    printf("===== 1. db_open + db_run_init_sql 建表 =====\n");
    CHECK(db_open(tmpdb, &db) == 0, "db_open 成功");
    CHECK(db_run_init_sql(db, "db/init.sql") == 0, "db_run_init_sql 执行 db/init.sql 成功");
    CHECK(db_run_init_sql(db, "./no_such_file.sql") == -1, "init.sql 不存在时返回-1（不崩）");
    CHECK(db_init_runtime(db) == 0, "db_init_runtime 补建表成功（IF NOT EXISTS）");

    printf("===== 2. 注册（明文进、加盐哈希落库） =====\n");
    CHECK(db_user_register(db, "hq", "pass1234") == 0, "注册用户 hq 成功");
    CHECK(db_user_register(db, "hq", "pass1234") == -1, "重复注册同名用户被拒（UNIQUE约束）");
    CHECK(db_user_register(db, "lisi", "abc654321") == 0, "注册第二个用户 lisi 成功");
    CHECK(db_user_register(db, "", "x") == -1, "空用户名被拒");
    {
        // 确认库里不是明文
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db, "SELECT password_hash FROM users WHERE username='hq';", -1, &st, NULL);
        const char *ph = (sqlite3_step(st) == SQLITE_ROW) ? (const char*)sqlite3_column_text(st,0) : "";
        printf("        库里存的: %s\n", ph);
        CHECK(strstr(ph, "pass1234") == NULL, "库里没有明文密码");
        CHECK(strchr(ph, '$') != NULL, "库里是 salt$hash 格式");
        sqlite3_finalize(st);
    }

    printf("===== 3. 登录校验 =====\n");
    int uid = -1;
    CHECK(db_user_login(db, "hq", "pass1234", &uid) == 0 && uid > 0, "密码正确 -> 登录成功并拿到uid");
    printf("        uid = %d\n", uid);
    CHECK(db_user_login(db, "hq", "wrongpass", &uid) == -1, "密码错误 -> 登录失败");
    CHECK(db_user_login(db, "nobody", "pass1234", &uid) == -1, "账号不存在 -> 登录失败");
    CHECK(db_user_login(db, "lisi", "abc654321", &uid) == 0, "第二个用户也能登录");

    printf("===== 4. 插入采集记录 =====\n");
    CHECK(db_insert_sensor_record(db, 25.3f, 61.2f, 1) == 0, "插入第1条采集记录");
    CHECK(db_insert_sensor_record(db, 26.1f, 59.8f, 1) == 0, "插入第2条采集记录");
    CHECK(db_insert_sensor_record(db, 33.0f, 58.0f, 2) == 0, "插入第3条(超阈值故障)记录");

    printf("===== 5. 查询 =====\n");
    char json[4096] = {0};
    int n = 0;
    CHECK(db_count_sensor_records(db, &n) == 0 && n == 3, "统计记录条数 = 3");
    CHECK(db_query_sensor_history(db, json, sizeof(json), 10) == 0, "查最近N条成功");
    CHECK(json[0] == '[' && strstr(json, "temp") != NULL, "返回合法JSON数组");
    printf("        最近N条: %s\n", json);
    CHECK(db_query_history_hours(db, json, sizeof(json), 24, 500) == 0, "按24小时跨度查询成功");
    CHECK(json[0] == '[' && strstr(json, "\"time\"") != NULL, "跨度查询含 time 字段（前端图表用）");
    printf("        24小时内: %s\n", json);

    printf("===== 6. 分级日志 =====\n");
    db_log(db, "INFO", "webserver", "用户登录成功: %s", "hq");
    db_log(db, "WARN", "collect", "温度超阈值: %.1f℃", 33.0);
    db_log(db, "ERROR", "modbus", "读取寄存器失败: %s", "Connection timed out");
    CHECK(db_query_logs(db, json, sizeof(json), 10) == 0, "查询日志成功");
    CHECK(strstr(json, "WARN") != NULL && strstr(json, "ERROR") != NULL, "日志里能看到级别字段");
    printf("        日志: %s\n", json);

    printf("===== 7. 登录会话（存库里，webserver 重启不掉登录） =====\n");
    {
        char sname[32] = {0}, srole[16] = {0};
        CHECK(db_session_create(db, "tok_test_abcdef1234567890", "hq", "admin", 3600) == 0,
              "创建会话成功（写进 sessions 表）");
        CHECK(db_session_check(db, "tok_test_abcdef1234567890", sname, sizeof(sname), srole, sizeof(srole)) == 0,
              "校验有效会话成功");
        CHECK(strcmp(sname, "hq") == 0 && strcmp(srole, "admin") == 0, "校验能带出用户名和角色");
        CHECK(db_session_check(db, "不存在的token", sname, sizeof(sname), NULL, 0) == -1,
              "无效 token 校验失败");
        CHECK(db_session_create(db, "tok_expired", "hq", "admin", -10) == 0, "创建一个已过期会话");
        CHECK(db_session_check(db, "tok_expired", sname, sizeof(sname), NULL, 0) == -1,
              "过期会话校验失败（拒绝过期）");
        CHECK(db_session_delete(db, "tok_test_abcdef1234567890") == 0, "删除会话（退出登录）");
        CHECK(db_session_check(db, "tok_test_abcdef1234567890", sname, sizeof(sname), NULL, 0) == -1,
              "删除后旧 token 立刻失效");
        db_session_cleanup(db);
        CHECK(db_session_check(db, "tok_expired", sname, sizeof(sname), NULL, 0) == -1,
              "过期会话被清理后依旧校验失败");
    }

    db_close(db);

    printf("\n===== 汇总：PASS=%d  FAIL=%d =====\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
