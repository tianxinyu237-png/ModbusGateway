/***********************************************************************************
 Copy right:        hqyj Tech.
 Description:    RESTful 接口层 —— 网页(ModbusGateway)用的 8 个接口都在这儿
                 POST /api/register            用户注册（密码加盐哈希入库）
                 POST /api/login               登录，返回 token
                 POST /api/logout              退出，token 立即失效
                 GET  /api/realtime            实时温湿度（读共享内存，加读锁）
                 GET  /api/history?hours=24    历史数据（SQLite，前端图表用）
                 POST /api/command             指令下发（消息队列 → 采集进程）
                 GET  /api/status              各进程运行状态
                 GET  /api/logs                分级运行日志
                 统一响应：{"code":0,"message":"ok","data":{...}}
                 除注册/登录外都要请求头 Authorization: Bearer *** 否则回 HTTP 401
                 注意：本层自己发送完整HTTP响应（含状态行），所以 thttpd.c 对 /api/ 开头的
                       请求不会再预先发 200 状态行
 ***********************************************************************************/
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include "api_rest.h"
#include "ipc/shm.h"     // 读/写共享内存
#include "ipc/mq.h"      // 往消息队列下发指令
#include "db/db.h"       // 用户/历史/日志

static sqlite3 *g_db = NULL;
sqlite3 *web_get_db(void)
{
    if(g_db == NULL)
    {
        if(db_open("./sensor.db", &g_db) != 0) return NULL;
        db_init_runtime(g_db);      // 补建 logs 等表（IF NOT EXISTS，不动老数据）
    }
    return g_db;
}

int web_get_int_param(const char *s, const char *key, int def)
{
    if(s == NULL) return def;
    const char *p = strstr(s, key);
    if(p == NULL) return def;
    return atoi(p + strlen(key));
}

/* ============================================================================
   三、RESTful 接口 /api/xxx（给 ModbusGateway 前端用）
       统一响应格式： {"code":0,"message":"ok","data":{...}}
       统一走 JSON 请求体；除注册/登录外都需要 Authorization: Bearer <token>
   ============================================================================ */
#define SESSION_TTL_SEC 7200

static void gen_token(char *out, int out_len)
{
    unsigned char b[16];
    static const char *hexd = "0123456789abcdef";
    FILE *fp = fopen("/dev/urandom","rb");
    int i;

    if(fp != NULL && fread(b,1,sizeof(b),fp) == sizeof(b)) fclose(fp);
    else {
        if(fp) fclose(fp);
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
        for(i=0;i<16;i++){ seed = seed*1103515245u + 12345u; b[i] = (unsigned char)(seed >> 16); }
    }
    for(i=0;i<16 && (i*2+2)<out_len;i++)
    {
        out[i*2]   = hexd[b[i] >> 4];
        out[i*2+1] = hexd[b[i] & 0x0f];
    }
    out[32] = '\0';
}

/* 会话统一存数据库（db_session_*），不放在进程内存里：
   这样 webserver 重启后 token 依然有效，同时开 80/8080 两个实例也能互相认账 */
static void session_create(const char *user, const char *role, char *out, int out_len)
{
    sqlite3 *db = web_get_db();
    gen_token(out, out_len);
    if(db == NULL) return;                  // 库打不开就只返回token，校验时自然失败
    db_session_cleanup(db);                 // 顺手清过期会话
    db_session_create(db, out, user, role, SESSION_TTL_SEC);
}

// 校验token：成功返回0，并把用户名带出去
static int session_check(const char *token, char *user_out, int out_len)
{
    sqlite3 *db = web_get_db();
    if(db == NULL) return -1;
    return db_session_check(db, token, user_out, out_len, NULL, 0);
}

static void session_remove(const char *token)
{
    sqlite3 *db = web_get_db();
    if(db != NULL && token != NULL) db_session_delete(db, token);
}

// 从 "Bearer xxxx" 里取出 token
static const char *bearer_token(const char *auth)
{
    if(auth == NULL) return NULL;
    if(strncasecmp(auth, "Bearer ", 7) == 0) return auth + 7;
    return auth;                             // 容错：直接把整个头当token
}

// 发完整响应（状态行 + 头 + 体），业务层自己控制状态码，401 才发得出去
static void send_json_status(int sock, int status, const char *json)
{
    char head[256];
    const char *reason = (status==200)?"OK":
                         (status==400)?"Bad Request":
                         (status==401)?"Unauthorized":
                         (status==404)?"Not Found":"Error";
    int n = snprintf(head,sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, reason, (int)strlen(json));
    send(sock, head, n, 0);
    send(sock, json, strlen(json), 0);
}

// 统一响应包 {"code":..,"message":..,"data":..}
static void send_wrapped(int sock, int status, int code, const char *msg, const char *data_json)
{
    cJSON *root = cJSON_CreateObject();
    char *s;

    cJSON_AddNumberToObject(root, "code", code);
    if(msg) cJSON_AddStringToObject(root, "message", msg);
    if(data_json)
    {
        cJSON *d = cJSON_Parse(data_json);
        if(d) cJSON_AddItemToObject(root, "data", d);
    }

    s = cJSON_PrintUnformatted(root);
    send_json_status(sock, status, s ? s : "{\"code\":-1,\"message\":\"json encode fail\"}");
    if(s) free(s);
    cJSON_Delete(root);
}

static const char *jstr(const cJSON *o, const char *k)
{
    cJSON *v;
    if(o == NULL) return NULL;
    v = cJSON_GetObjectItem((cJSON*)o, k);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    cJSON *v;
    if(o == NULL) return def;
    v = cJSON_GetObjectItem((cJSON*)o, k);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : def;
}

// 需要登录的接口，先过这道关；失败时直接回401
static int require_login(int sock, const char *auth, char *user_out, int out_len)
{
    const char *tk = bearer_token(auth);
    if(session_check(tk, user_out, out_len) != 0)
    {
        send_wrapped(sock, 401, 401, "未登录或登录已过期", NULL);
        return -1;
    }
    return 0;
}

/* ---------- 1) 注册 ---------- */
static int api_register(int sock, cJSON *body)
{
    const char *u = jstr(body, "username");
    const char *p = jstr(body, "password");
    sqlite3 *db = web_get_db();

    if(u == NULL || p == NULL || u[0]=='\0' || p[0]=='\0')
    {
        send_wrapped(sock, 200, 1003, "用户名或密码不能为空", NULL);
        return 0;
    }
    if(db == NULL)
    {
        send_wrapped(sock, 200, 1005, "数据库打不开", NULL);
        return 0;
    }
    if(db_user_register(db, u, p) != 0)
    {
        db_log(db, "WARN", "webserver", "注册失败(用户名已存在?): %s", u);
        send_wrapped(sock, 200, 1002, "用户名已存在", NULL);
        return 0;
    }

    db_log(db, "INFO", "webserver", "新用户注册: %s → 写入用户信息表", u);
    printf("register ok: %s\n", u);
    send_wrapped(sock, 200, 0, "注册成功", NULL);
    return 0;
}

/* ---------- 2) 登录 ---------- */
static int api_login(int sock, cJSON *body)
{
    const char *u = jstr(body, "username");
    const char *p = jstr(body, "password");
    sqlite3 *db = web_get_db();
    int uid = -1;
    char data[256];

    if(u == NULL || p == NULL)
    {
        send_wrapped(sock, 200, 1003, "用户名或密码不能为空", NULL);
        return 0;
    }
    if(db == NULL || db_user_login(db, u, p, &uid) != 0)
    {
        if(db) db_log(db, "WARN", "webserver", "登录失败: %s (密码错误或用户不存在)", u);
        send_wrapped(sock, 200, 1001, "用户名或密码错误", NULL);
        return 0;
    }

    const char *role = (uid == 1 || strcmp(u, "admin") == 0) ? "admin" : "user";
    char token[40];
    session_create(u, role, token, sizeof(token));      // 会话入库，重启不掉
    snprintf(data, sizeof(data),
        "{\"username\":\"%s\",\"role\":\"%s\",\"uid\":%d,\"token\":\"%s\"}",
        u, role, uid, token);

    db_log(db, "INFO", "webserver", "用户登录成功: %s", u);
    printf("login ok: %s token=%s\n", u, token);
    send_wrapped(sock, 200, 0, "登录成功", data);
    return 0;
}

/* ---------- 3) 退出 ---------- */
static int api_logout(int sock, const char *auth)
{
    const char *tk = bearer_token(auth);
    char user[32] = {0};
    if(session_check(tk, user, sizeof(user)) == 0)
    {
        sqlite3 *db = web_get_db();
        if(db) db_log(db, "INFO", "webserver", "用户退出登录: %s", user);
    }
    session_remove(tk);
    send_wrapped(sock, 200, 0, "已退出登录", NULL);
    return 0;
}

/* ---------- 4) 实时数据（读共享内存，加读锁） ---------- */
static int api_realtime(int sock)
{
    shm_sensor_data_t *shm = NULL;
    float t, h, tmax, tmin, th;
    unsigned int cnt;
    time_t ts, start;
    int st, sid, pm, src_sim;
    char timestr[32] = "-";
    char src_name[48] = "-", src_target[64] = "-";
    char data[768];

    if(shm_sensor_attach(&shm) != 0)
    {
        // 采集进程没起来：返回业务错误码，前端会提示，不是崩
        send_wrapped(sock, 200, 2001, "采集进程未运行(共享内存不存在)", NULL);
        return 0;
    }

    if(shm_rdlock(shm, 300) != 0)
    {
        shm_sensor_detach(shm);
        send_wrapped(sock, 200, 2002, "读取共享内存超时", NULL);
        return 0;
    }
    t    = shm->temperature;
    h    = shm->humidity;
    tmax = shm->temp_max;
    tmin = shm->temp_min;
    th   = shm->threshold;
    cnt  = shm->sample_count;
    ts   = shm->timestamp;
    start= shm->start_time;
    st   = shm->device_status;
    sid  = shm->slave_id;
    pm   = shm->period_ms;
    src_sim = shm->source_simulated;
    snprintf(src_name,   sizeof(src_name),   "%s", shm->source_name[0] ? shm->source_name : "-");
    snprintf(src_target, sizeof(src_target), "%s", shm->device_target[0] ? shm->device_target : "-");
    shm_unlock(shm);
    shm_sensor_detach(shm);

    if(ts > 0)
    {
        struct tm tmv;
        localtime_r(&ts, &tmv);
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tmv);
    }

    // 数据新鲜度：超过 3 个采集周期(至少5秒)没更新就算离线
    int fresh = (pm > 0) ? (pm/1000)*3 + 5 : 5;
    int online = (ts > 0 && (time(NULL) - ts) <= fresh && st != 0);

    snprintf(data, sizeof(data),
        "{\"temp\":%.2f,\"humi\":%.2f,\"tempMax\":%.2f,\"tempMin\":%.2f,"
        "\"count\":%u,\"slaveId\":%d,\"online\":%s,\"collectTime\":\"%s\","
        "\"period\":%d,\"threshold\":%.1f,\"startTime\":%ld,"
        "\"source\":{\"name\":\"%s\",\"simulated\":%s,\"target\":\"%s\",\"status\":\"%s\"}}",
        t, h, tmax, tmin, cnt, sid, online?"true":"false", timestr, pm, th, (long)start,
        src_name, src_sim?"true":"false", src_target,
        src_sim ? "模拟数据" : (online ? "真实数据" : "真实设备(离线)"));

    send_wrapped(sock, 200, 0, "ok", data);
    return 0;
}

/* ---------- 5) 历史数据（SQLite，按小时跨度） ---------- */
static int api_history(int sock, const char *query_string)
{
    // 【踩过的坑】这里原来用 char json[8192] 固定缓冲区，数据一多（150+行）就被截断，
    // 截断后的JSON解析失败，前端拿到的是空列表。历史点数是不定的，必须动态分配。
    // 单次最多返回 HISTORY_MAX_POINTS 个点（图表不需要几万个点）
    #define HISTORY_MAX_POINTS 1000
    #define HISTORY_BUF_SIZE   (256 * 1024)

    int hours = web_get_int_param(query_string, "hours=", 24);
    int limit = web_get_int_param(query_string, "limit=", HISTORY_MAX_POINTS);
    char *json = NULL;
    char *data = NULL;
    sqlite3 *db = web_get_db();

    if(hours < 1 || hours > 168) hours = 24;
    if(limit < 1 || limit > HISTORY_MAX_POINTS) limit = HISTORY_MAX_POINTS;

    json = malloc(HISTORY_BUF_SIZE);
    data = malloc(HISTORY_BUF_SIZE + 128);
    if(json == NULL || data == NULL)
    {
        free(json); free(data);
        send_wrapped(sock, 200, 2007, "服务器内存不足", NULL);
        return 0;
    }
    json[0] = '\0';

    if(db == NULL || db_query_history_hours(db, json, HISTORY_BUF_SIZE, hours, limit) != 0)
    {
        free(json); free(data);
        send_wrapped(sock, 200, 2003, "查询历史失败(sensor.db或表不存在)", NULL);
        return 0;
    }

    snprintf(data, HISTORY_BUF_SIZE + 128, "{\"hours\":%d,\"limit\":%d,\"list\":%s}",
             hours, limit, (json[0] == '\0') ? "[]" : json);
    send_wrapped(sock, 200, 0, "ok", data);

    free(json);
    free(data);
    return 0;
}

/* ---------- 6) 指令下发（消息队列） ---------- */
static int api_command(int sock, cJSON *body)
{
    const char *action = jstr(body, "action");
    sqlite3 *db = web_get_db();
    mqd_t mqfd;
    int rc;
    char data[512];

    if(action == NULL)
    {
        send_wrapped(sock, 200, 1004, "缺少action参数", NULL);
        return 0;
    }

    mqfd = mq_modbus_open(0);        // 只打开已存在的队列
    if(mqfd == (mqd_t)-1)
    {
        send_wrapped(sock, 200, 2004, "消息队列打不开，采集进程可能没在运行", NULL);
        return 0;
    }

    if(strcmp(action, "setPeriod") == 0)
    {
        int ms = (int)jnum(body, "period", 2000);
        if(ms < 100 || ms > 60000)
        {
            mq_modbus_close(mqfd);
            send_wrapped(sock, 200, 1004, "period 应在 100~60000 ms 之间", NULL);
            return 0;
        }
        rc = mq_send_cmd(mqfd, CMD_SET_INTERVAL, ms);
        mq_modbus_close(mqfd);
        if(db) db_log(db, "INFO", "mq", "下发采集周期: %d ms (rc=%d)", ms, rc);
        snprintf(data, sizeof(data), "{\"action\":\"setPeriod\",\"period\":%d,\"send_rc\":%d}", ms, rc);
        send_wrapped(sock, 200, (rc==0)?0:2005, (rc==0)?"已下发采集周期指令 → 消息队列":"下发失败", data);
        return 0;
    }

    if(strcmp(action, "setThreshold") == 0)
    {
        int v = (int)(jnum(body, "threshold", 32) * 100);      // 用整数传小数：32.5℃ -> 3250
        rc = mq_send_cmd(mqfd, CMD_SET_THRESHOLD, v);
        mq_modbus_close(mqfd);
        if(db) db_log(db, "INFO", "mq", "下发温度告警阈值: %.1f℃ (rc=%d)", v/100.0, rc);
        snprintf(data, sizeof(data), "{\"action\":\"setThreshold\",\"threshold\":%.1f,\"send_rc\":%d}", v/100.0, rc);
        send_wrapped(sock, 200, (rc==0)?0:2005, (rc==0)?"已下发温度告警阈值指令":"下发失败", data);
        return 0;
    }

    if(strcmp(action, "readRegister") == 0)
    {
        int addr  = (int)jnum(body, "addr", 0);
        int count = (int)jnum(body, "count", 2);
        if(addr < 0 || addr > 1000 || count < 1 || count > 8)
        {
            mq_modbus_close(mqfd);
            send_wrapped(sock, 200, 1004, "addr/count 不合法(count 1~8)", NULL);
            return 0;
        }
        rc = mq_send_cmd(mqfd, CMD_QUERY, (addr << 16) | count);
        mq_modbus_close(mqfd);
        if(db) db_log(db, "INFO", "modbus", "下发读取寄存器: addr=%d count=%d (rc=%d)", addr, count, rc);

        // 等采集进程读回来，再从共享内存取结果（最多等300ms）
        usleep(300 * 1000);
        shm_sensor_data_t *shm = NULL;
        if(shm_sensor_attach(&shm) == 0 && shm_rdlock(shm, 200) == 0)
        {
            char regs[256] = {0};
            int i;
            for(i=0;i<shm->last_regs_count && i<8;i++)
            {
                char one[24];
                snprintf(one,sizeof(one),"%s%u", i?",":"", shm->last_regs[i]);
                strcat(regs, one);
            }
            snprintf(data, sizeof(data),
                "{\"action\":\"readRegister\",\"addr\":%d,\"count\":%d,\"regs\":[%s],\"regsAddr\":%d}",
                addr, count, regs, shm->last_regs_addr);
            shm_unlock(shm);
            shm_sensor_detach(shm);
            send_wrapped(sock, 200, 0, "已下发读取寄存器指令并取回结果", data);
        }
        else
        {
            snprintf(data, sizeof(data), "{\"action\":\"readRegister\",\"addr\":%d,\"count\":%d}", addr, count);
            send_wrapped(sock, 200, 0, "已下发读取寄存器指令（未取到回值）", data);
        }
        return 0;
    }

    if(strcmp(action, "restart") == 0)
    {
        mq_modbus_close(mqfd);
        if(db) db_log(db, "WARN", "webserver", "收到重启指令（当前版本需手动/脚本重启采集进程）");
        // 说明：真正的"异常自愈、自动拉起"要靠守护/脚本，属于后续阶段，这里不假装能重启
        send_wrapped(sock, 200, 0, "重启指令已记录：当前版本请手动重启 collector.out（自动拉起待守护进程阶段实现）", NULL);
        return 0;
    }

    mq_modbus_close(mqfd);
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "未知指令: %s", action);
        send_wrapped(sock, 200, 1004, msg, NULL);
    }
    return 0;
}

/* ---------- 7) 运行状态 ---------- */
static int api_status(int sock)
{
    shm_sensor_data_t *shm = NULL;
    int collect_online = 0, modbus_online = 0, period = 0, uptime = 0, st = 0, sid = 0, src_sim = 0;
    int has_shm = 0;
    long ts = 0;
    char src_json[256];
    char src_name[48] = "-", src_target[64] = "-";
    char data[720];

    if(shm_sensor_attach(&shm) == 0)
    {
        if(shm_rdlock(shm, 300) == 0)
        {
            time_t now = time(NULL);
            ts = (long)shm->timestamp;
            st = shm->device_status;
            sid = shm->slave_id;
            src_sim = shm->source_simulated;
            snprintf(src_name,   sizeof(src_name),   "%s", shm->source_name[0] ? shm->source_name : "-");
            snprintf(src_target, sizeof(src_target), "%s", shm->device_target[0] ? shm->device_target : "-");
            period = shm->period_ms;
            if(shm->start_time > 0) uptime = (int)(now - shm->start_time);
            int fresh = (period > 0) ? (period/1000)*3 + 5 : 5;
            collect_online = (ts > 0 && (now - ts) <= fresh);
            modbus_online  = (st == 1);
            has_shm = 1;
            shm_unlock(shm);
        }
        shm_sensor_detach(shm);
    }

    // 采集进程没跑时不能瞎报数据来源（不然后端会说"真实设备"，误导人）
    if(has_shm)
        snprintf(src_json, sizeof(src_json),
                 "{\"name\":\"%s\",\"simulated\":%s,\"target\":\"%s\"}",
                 src_name, src_sim?"true":"false", src_target);
    else
        snprintf(src_json, sizeof(src_json), "null");

    snprintf(data, sizeof(data),
        "{\"collectOnline\":%s,\"webOnline\":true,\"modbusOnline\":%s,"
        "\"period\":%d,\"uptime\":%d,\"lastCollectTs\":%ld,\"deviceStatus\":%d,\"slaveId\":%d,"
        "\"source\":%s}",
        collect_online?"true":"false", modbus_online?"true":"false",
        period, uptime, ts, st, sid, src_json);

    send_wrapped(sock, 200, 0, "ok", data);
    return 0;
}

/* ---------- 8) 运行日志 ---------- */
static int api_logs(int sock, const char *query_string)
{
    // 同样不能用小的固定缓冲区，日志条数不定，动态分配
    #define LOGS_BUF_SIZE (128 * 1024)
    int limit = web_get_int_param(query_string, "limit=", 60);
    char *json = NULL;
    char *data = NULL;
    sqlite3 *db = web_get_db();

    if(limit < 1 || limit > 200) limit = 60;

    json = malloc(LOGS_BUF_SIZE);
    data = malloc(LOGS_BUF_SIZE + 64);
    if(json == NULL || data == NULL)
    {
        free(json); free(data);
        send_wrapped(sock, 200, 2007, "服务器内存不足", NULL);
        return 0;
    }
    json[0] = '\0';

    if(db == NULL || db_query_logs(db, json, LOGS_BUF_SIZE, limit) != 0)
    {
        free(json); free(data);
        send_wrapped(sock, 200, 2006, "查询日志失败(logs表不存在?)", NULL);
        return 0;
    }

    snprintf(data, LOGS_BUF_SIZE + 64, "{\"list\":%s}", (json[0]=='\0') ? "[]" : json);
    send_wrapped(sock, 200, 0, "ok", data);

    free(json);
    free(data);
    return 0;
}

// REST 路由分发：url 形如 /api/realtime（声明在 api_rest.h，供 custom_handle.c 调用）
int api_dispatch(int sock, const char *url, const char *query_string,
                        const char *input, const char *auth)
{
    cJSON *body = NULL;
    char user[32] = {0};
    int ret = 0;

    // JSON 请求体（GET 时 input 是空串）
    if(input != NULL && input[0] != '\0' && input[0] == '{')
    {
        body = cJSON_Parse(input);
        if(body == NULL) printf("[api] JSON解析失败: %.80s\n", input);
    }

    if(strcmp(url, "/api/register") == 0)
    {
        ret = api_register(sock, body);
    }
    else if(strcmp(url, "/api/login") == 0)
    {
        ret = api_login(sock, body);
    }
    else if(strcmp(url, "/api/logout") == 0)
    {
        ret = api_logout(sock, auth);         // 退出不强制要求有效token，清了就行
    }
    // 下面这些必须先登录
    else if(require_login(sock, auth, user, sizeof(user)) != 0)
    {
        ret = 0;                              // 已经回了401
    }
    else if(strcmp(url, "/api/realtime") == 0)
    {
        ret = api_realtime(sock);
    }
    else if(strcmp(url, "/api/history") == 0)
    {
        ret = api_history(sock, query_string);
    }
    else if(strcmp(url, "/api/command") == 0)
    {
        ret = api_command(sock, body);
    }
    else if(strcmp(url, "/api/status") == 0)
    {
        ret = api_status(sock);
    }
    else if(strcmp(url, "/api/logs") == 0)
    {
        ret = api_logs(sock, query_string);
    }
    else
    {
        send_wrapped(sock, 404, 404, "接口不存在", NULL);
    }

    if(body) cJSON_Delete(body);
    return ret;
}

