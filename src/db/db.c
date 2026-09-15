#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>

/* ============================================================================
   极简 SHA-256 实现（纯C，不依赖 openssl/libcrypto，方便在实验机上直接编译）
   自我验证：sha256("")    = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
             sha256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
   可与命令行 `echo -n abc | sha256sum` 对照
   ============================================================================ */
typedef struct {
    unsigned int h[8];
    unsigned long long total;
    unsigned char buf[64];
    unsigned int buflen;
} sha256_ctx;

static const unsigned int SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR32(x,n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH32(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ32(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR32(x,2)  ^ ROTR32(x,13) ^ ROTR32(x,22))
#define BSIG1(x) (ROTR32(x,6)  ^ ROTR32(x,11) ^ ROTR32(x,25))
#define SSIG0(x) (ROTR32(x,7)  ^ ROTR32(x,18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR32(x,17) ^ ROTR32(x,19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx *c, const unsigned char *data)
{
    unsigned int w[64], a,b,cc,d,e,f,g,h,t1,t2;
    int i;

    for(i=0;i<16;i++)
        w[i] = ((unsigned int)data[i*4]   << 24) | ((unsigned int)data[i*4+1] << 16) |
               ((unsigned int)data[i*4+2] <<  8) | ((unsigned int)data[i*4+3]);

    for(i=16;i<64;i++)
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];

    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6];  h=c->h[7];

    for(i=0;i<64;i++)
    {
        t1 = h + BSIG1(e) + CH32(e,f,g) + SHA256_K[i] + w[i];
        t2 = BSIG0(a) + MAJ32(a,b,cc);
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }

    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g;  c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total=0; c->buflen=0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char*)data;
    c->total += len;
    while(len > 0)
    {
        unsigned int n = 64 - c->buflen;
        if(n > len) n = (unsigned int)len;
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n;
        p += n;
        len -= n;
        if(c->buflen == 64)
        {
            sha256_transform(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    unsigned long long bits = c->total * 8;   // 注意：要在补位之前算
    unsigned char pad = 0x80, zero = 0x00, lb[8];
    int i;

    sha256_update(c, &pad, 1);
    while(c->buflen != 56) sha256_update(c, &zero, 1);
    for(i=0;i<8;i++) lb[i] = (unsigned char)(bits >> (56 - 8*i));
    sha256_update(c, lb, 8);

    for(i=0;i<8;i++)
    {
        out[i*4+0] = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >>  8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

/* 生成一段随机盐（16个十六进制字符），优先 /dev/urandom */
static int make_salt(char *out, int out_len)
{
    unsigned char b[8];
    FILE *fp = fopen("/dev/urandom", "rb");
    if(fp != NULL)
    {
        size_t n = fread(b, 1, sizeof(b), fp);
        fclose(fp);
        if(n != sizeof(b)) return -1;
    }
    else
    {
        // 兜底（不够随机，但总比没有盐好）
        int i; unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
        for(i=0;i<8;i++){ seed = seed*1103515245u + 12345u; b[i] = (unsigned char)(seed >> 16); }
    }
    static const char *hexd = "0123456789abcdef";
    int i;
    for(i=0;i<8;i++)
    {
        if(i*2+2 >= out_len) break;
        out[i*2]   = hexd[b[i] >> 4];
        out[i*2+1] = hexd[b[i] & 0x0f];
    }
    out[16] = '\0';
    return 0;
}

int db_make_hash(const char *password, const char *salt, char *hex_out, int hex_len)
{
    sha256_ctx c;
    unsigned char d[32];
    static const char *hexd = "0123456789abcdef";
    int i;

    if(password == NULL || salt == NULL || hex_out == NULL || hex_len < 65) return -1;

    sha256_init(&c);
    sha256_update(&c, salt, strlen(salt));
    sha256_update(&c, password, strlen(password));
    sha256_final(&c, d);

    for(i=0;i<32;i++)
    {
        hex_out[i*2]   = hexd[d[i] >> 4];
        hex_out[i*2+1] = hexd[d[i] & 0x0f];
    }
    hex_out[64] = '\0';
    return 0;
}

/* ============================================================================
   数据库打开 / 建表 / 关闭
   ============================================================================ */
int db_open(const char *db_path, sqlite3 **pdb)
{
    int rc = sqlite3_open(db_path, pdb);
    if(rc != SQLITE_OK){
        fprintf(stderr,"db open fail: %s\n", sqlite3_errmsg(*pdb));
        return -1;
    }
    // 开启WAL、忙等待超时
    sqlite3_exec(*pdb, "PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000;", NULL,NULL,NULL);
    return 0;
}

// 执行初始化脚本 init.sql（建 users / sensor_data / logs 表，并设置WAL等PRAGMA）
int db_run_init_sql(sqlite3 *db, const char *sql_file)
{
    FILE *fp = fopen(sql_file, "rb");
    if(fp == NULL)
    {
        fprintf(stderr,"init sql open fail: %s\n", sql_file);
        return -1;
    }

    // 一次性读完整个脚本，这里给64KB上限，教学脚本足够了
    char *sql = malloc(64*1024);
    if(sql == NULL)
    {
        fclose(fp);
        return -1;
    }
    size_t len = fread(sql, 1, 64*1024-1, fp);
    sql[len] = '\0';
    fclose(fp);

    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK)
    {
        fprintf(stderr,"init sql exec fail: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        free(sql);
        return -1;
    }
    free(sql);
    return 0;
}

// 运行期补建表：只 CREATE IF NOT EXISTS，绝不 DROP，程序启动时调一次
int db_init_runtime(sqlite3 *db)
{
    if(db == NULL) return -1;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS users("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS sensor_data("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  temperature REAL NOT NULL,"
        "  humidity REAL NOT NULL,"
        "  device_status INTEGER DEFAULT 1,"
        "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS logs("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  time DATETIME DEFAULT (datetime('now','localtime')),"
        "  level TEXT NOT NULL,"
        "  module TEXT,"
        "  message TEXT);"
        "CREATE TABLE IF NOT EXISTS sessions("
        "  token TEXT PRIMARY KEY,"
        "  username TEXT NOT NULL,"
        "  role TEXT,"
        "  expire INTEGER NOT NULL,"       /* unix 时间戳，方便比较 */
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP);";

    char *errmsg = NULL;
    if(sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK)
    {
        fprintf(stderr,"db_init_runtime fail: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

void db_close(sqlite3 *db)
{
    if(db) sqlite3_close(db);
}

/* ============================================================================
   用户系统
   ============================================================================ */
// 注册：内部生成随机盐并哈希，库里存 "salt$hash"
int db_user_register(sqlite3 *db, const char *username, const char *password)
{
    char salt[20], hash[65], stored[96];
    sqlite3_stmt *stmt;
    int rc;

    if(db == NULL || username == NULL || password == NULL) return -1;
    if(username[0] == '\0' || password[0] == '\0') return -1;

    if(make_salt(salt, sizeof(salt)) != 0) return -1;
    if(db_make_hash(password, salt, hash, sizeof(hash)) != 0) return -1;
    snprintf(stored, sizeof(stored), "%s$%s", salt, hash);

    const char *sql = "INSERT INTO users(username,password_hash) VALUES(?,?);";
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt,1,username,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,stored,-1,SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;    // 用户名重复会被 UNIQUE 约束拒绝
}

// 登录校验：按用户名取出盐，用同样规则算哈希比对，成功返回0并把uid带出来
int db_user_login(sqlite3 *db, const char *username, const char *password, int *uid)
{
    sqlite3_stmt *stmt;
    int rc, ret = -1;
    char stored[128], salt[40], expect[65];

    if(db == NULL || username == NULL || password == NULL) return -1;

    const char *sql = "SELECT id, password_hash FROM users WHERE username=?;";
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt,1,username,-1,SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);

    if(rc == SQLITE_ROW)
    {
        const unsigned char *ph = sqlite3_column_text(stmt,1);
        snprintf(stored, sizeof(stored), "%s", ph ? (const char*)ph : "");
        char *sep = strchr(stored, '$');
        if(sep != NULL)
        {
            *sep = '\0';
            snprintf(salt, sizeof(salt), "%s", stored);          // '$' 前面是盐
            if(db_make_hash(password, salt, expect, sizeof(expect)) == 0
               && strcmp(expect, sep+1) == 0)                     // '$' 后面是哈希
            {
                if(uid != NULL) *uid = sqlite3_column_int(stmt,0);
                ret = 0;
            }
        }
    }
    sqlite3_finalize(stmt);
    return ret;
}

/* ============================================================================
   采集数据
   ============================================================================ */
// 插入传感器记录，给collector采集进程调用
int db_insert_sensor_record(sqlite3 *db, float temp, float humi, int status)
{
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO sensor_data(temperature,humidity,device_status) VALUES(?,?,?);";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) return -1;

    sqlite3_bind_double(stmt,1,temp);
    sqlite3_bind_double(stmt,2,humi);
    sqlite3_bind_int(stmt,3,status);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

// 查询历史，输出cJSON字符串，给web api接口调用（字段 temp/humi/status/time）
int db_query_sensor_history(sqlite3 *db, char *out_json, int out_len, int limit)
{
    // 【必须判】句柄和参数先校验；失败时把输出清空，调用方就不会拿到上一次的脏数据
    if(db == NULL || out_json == NULL || out_len <= 0) return -1;
    out_json[0] = '\0';
    if(limit < 1) limit = 10;      // LIMIT 传0或负数查不到任何行，给个默认值

    cJSON *root = cJSON_CreateArray();
    if(root == NULL) return -1;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT temperature,humidity,device_status,"
                      "strftime('%Y-%m-%d %H:%M:%S', timestamp, 'localtime') "
                      "FROM sensor_data ORDER BY id DESC LIMIT ?;";
    int rc = sqlite3_prepare_v2(db,sql,-1,&stmt,NULL);
    if(rc != SQLITE_OK)
    {
        // 常见原因：sensor_data 表还没建（先执行一次 db/init.sql）
        fprintf(stderr,"query sensor history prepare fail: %s\n", sqlite3_errmsg(db));
        cJSON_Delete(root);
        return -1;
    }

    sqlite3_bind_int(stmt,1,limit);
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item,"temp", sqlite3_column_double(stmt,0));
        cJSON_AddNumberToObject(item,"humi", sqlite3_column_double(stmt,1));
        cJSON_AddNumberToObject(item,"status", sqlite3_column_int(stmt,2));
        cJSON_AddStringToObject(item,"time", (const char*)sqlite3_column_text(stmt,3));
        cJSON_AddItemToArray(root,item);
    }
    sqlite3_finalize(stmt);

    char *json_buf = cJSON_PrintUnformatted(root);
    if(json_buf != NULL)
    {
        strncpy(out_json, json_buf, out_len-1);
        out_json[out_len-1] = '\0';   // 保证字符串结尾，否则前端拿到的是脏数据
        free(json_buf);
    }
    else
    {
        out_json[0] = '\0';
    }
    cJSON_Delete(root);
    return 0;
}

// 按时间跨度查历史：最近 hours 小时，最多 limit 条，输出为时间升序（给图表）
int db_query_history_hours(sqlite3 *db, char *out_json, int out_len, int hours, int limit)
{
    if(db == NULL || out_json == NULL || out_len <= 0) return -1;
    out_json[0] = '\0';
    if(hours < 1) hours = 24;
    if(limit < 1 || limit > 20000) limit = 5000;

    cJSON *root = cJSON_CreateArray();
    if(root == NULL) return -1;

    char mod[32];
    snprintf(mod, sizeof(mod), "-%d hours", hours);

    // 先按时间倒序取最新 limit 条，再外层升序，保证图表是从旧到新
    const char *sql =
        "SELECT time, temperature, humidity FROM ("
        "  SELECT strftime('%Y-%m-%d %H:%M:%S', timestamp, 'localtime') AS time,"
        "         temperature, humidity, id"
        "  FROM sensor_data WHERE timestamp >= datetime('now', ?) "
        "  ORDER BY id DESC LIMIT ?"
        ") ORDER BY id ASC;";

    sqlite3_stmt *stmt;
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr,"query history hours prepare fail: %s\n", sqlite3_errmsg(db));
        cJSON_Delete(root);
        return -1;
    }

    sqlite3_bind_text(stmt,1,mod,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,2,limit);

    int rc;
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item,"time", (const char*)sqlite3_column_text(stmt,0));
        cJSON_AddNumberToObject(item,"temp", sqlite3_column_double(stmt,1));
        cJSON_AddNumberToObject(item,"humi", sqlite3_column_double(stmt,2));
        cJSON_AddItemToArray(root,item);
    }
    sqlite3_finalize(stmt);

    char *json_buf = cJSON_PrintUnformatted(root);
    if(json_buf != NULL)
    {
        strncpy(out_json, json_buf, out_len-1);
        out_json[out_len-1] = '\0';
        free(json_buf);
    }
    cJSON_Delete(root);
    return 0;
}

int db_count_sensor_records(sqlite3 *db, int *count)
{
    sqlite3_stmt *stmt;
    if(db == NULL || count == NULL) return -1;
    *count = 0;

    if(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sensor_data;", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if(sqlite3_step(stmt) == SQLITE_ROW) *count = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
    return 0;
}

/* ============================================================================
   登录会话（存数据库，webserver 重启不掉登录）
   ============================================================================ */
int db_session_create(sqlite3 *db, const char *token, const char *username,
                      const char *role, int ttl_sec)
{
    sqlite3_stmt *stmt;
    if(db == NULL || token == NULL || username == NULL) return -1;

    // 同一 token 重复登录就覆盖（REPLACE），不会主键冲突
    const char *sql = "INSERT OR REPLACE INTO sessions(token,username,role,expire) "
                      "VALUES(?,?,?,?);";
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt,1,token,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,username,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,role ? role : "user",-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,4,(sqlite3_int64)time(NULL) + (ttl_sec != 0 ? ttl_sec : 7200));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_session_check(sqlite3 *db, const char *token, char *user_out, int user_len,
                     char *role_out, int role_len)
{
    sqlite3_stmt *stmt;
    int ret = -1;

    if(db == NULL || token == NULL || token[0] == '\0') return -1;

    const char *sql = "SELECT username, role FROM sessions WHERE token=? AND expire > ?;";
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt,1,token,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,2,(sqlite3_int64)time(NULL));

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
        if(user_out && user_len > 0)
            snprintf(user_out, user_len, "%s", (const char*)sqlite3_column_text(stmt,0));
        if(role_out && role_len > 0)
            snprintf(role_out, role_len, "%s", (const char*)sqlite3_column_text(stmt,1));
        ret = 0;
    }
    sqlite3_finalize(stmt);
    return ret;
}

int db_session_delete(sqlite3 *db, const char *token)
{
    sqlite3_stmt *stmt;
    if(db == NULL || token == NULL) return -1;
    if(sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token=?;", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt,1,token,-1,SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

// 顺手清理过期会话（每次登录时调一次，免得表越攒越大）
int db_session_cleanup(sqlite3 *db)
{
    char sql[128];
    if(db == NULL) return -1;
    snprintf(sql, sizeof(sql), "DELETE FROM sessions WHERE expire <= %ld;", (long)time(NULL));
    return (sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
}

/* ============================================================================
   分级日志
   ============================================================================ */
int db_log(sqlite3 *db, const char *level, const char *module, const char *fmt, ...)
{
    char message[512];
    va_list ap;

    if(db == NULL || fmt == NULL) return -1;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO logs(time,level,module,message) "
                      "VALUES(datetime('now','localtime'),?,?,?);";
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt,1, level ? level : "INFO", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2, module ? module : "-", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3, message, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_query_logs(sqlite3 *db, char *out_json, int out_len, int limit)
{
    if(db == NULL || out_json == NULL || out_len <= 0) return -1;
    out_json[0] = '\0';
    if(limit < 1 || limit > 500) limit = 60;

    cJSON *root = cJSON_CreateArray();
    if(root == NULL) return -1;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT time, level, module, message FROM logs ORDER BY id DESC LIMIT ?;";
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        cJSON_Delete(root);
        return -1;
    }

    sqlite3_bind_int(stmt,1,limit);
    int rc;
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item,"time",   (const char*)sqlite3_column_text(stmt,0));
        cJSON_AddStringToObject(item,"level",  (const char*)sqlite3_column_text(stmt,1));
        cJSON_AddStringToObject(item,"module", (const char*)sqlite3_column_text(stmt,2));
        cJSON_AddStringToObject(item,"message",(const char*)sqlite3_column_text(stmt,3));
        cJSON_AddItemToArray(root,item);
    }
    sqlite3_finalize(stmt);

    char *json_buf = cJSON_PrintUnformatted(root);
    if(json_buf != NULL)
    {
        strncpy(out_json, json_buf, out_len-1);
        out_json[out_len-1] = '\0';
        free(json_buf);
    }
    cJSON_Delete(root);
    return 0;
}
