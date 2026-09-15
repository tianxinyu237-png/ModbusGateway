/***********************************************************************************
 Copy right:        hqyj Tech.
 Author:         jiaoyue
 Date:           2023.07.01
 Description:    http业务处理层
                 · 表单业务：/login（登录）、/add（求和），POST 表单
                 · 老接口  ：/api?cmd=realtime|interval|history（返回裸JSON，realtime.html 在用）
                 · 总入口  ：parse_and_process()，把 /api/xxx 转给 api_rest.c 的 api_dispatch()
                 RESTful 接口（网页 ModbusGateway 用的 8 个接口）在 api_rest.c 里
 ***********************************************************************************/
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "custom_handle.h"   // 头文件，对外声明parse_and_process函数，供thttpd.c调用
#include "api_rest.h"        // RESTful 接口层（/api/xxx，实现在 api_rest.c）
#include "src/ipc/shm.h"     // 读采集进程写进共享内存的实时温湿度（读写锁）
#include "src/ipc/mq.h"      // 往消息队列下发指令给采集进程
#include "src/db/db.h"       // 历史/日志查询

#define KB 1024
#define HTML_SIZE (64 * KB)     // 64KB 响应缓冲区大小

/* ============================================================================
   一、老的业务接口（登录 / 求和）—— 路径 /login、/add，POST表单
   ============================================================================ */
static int handle_login(int sock, const char *input)
{
    char reply_buf[HTML_SIZE] = {0};
    char *uname = strstr(input, "username=");
    char *p = strstr(input, "password");

    if(uname == NULL || p == NULL)
    {
        const char *err = "{\"error\":\"表单缺少username或password字段\"}";
        send(sock,err,strlen(err),0);
        return 0;
    }
    uname += strlen("username=");

    if(p <= input)
    {
        const char *err = "{\"error\":\"表单格式错误\"}";
        send(sock,err,strlen(err),0);
        return 0;
    }
    *(p - 1) = '\0';
    printf("username = %s\n", uname);

    char *passwd = p + strlen("password=");
    printf("passwd = %s\n", passwd);

    if(strcmp(uname, "admin")==0 && strcmp(passwd, "admin")==0)
    {
        sprintf(reply_buf, "<script>localStorage.setItem('usr_user_name', '%s');</script>", uname);
        strcat(reply_buf, "<script>window.location.href = '/index.html';</script>");
        send(sock,reply_buf,strlen(reply_buf),0);
    }
    else
    {
        printf("web login failed\n");
        char out[128] = {0xd3,0xc3,0xbb,0xa7,0xc3,0xfb,0xbb,0xf2,0xc3,0xdc,0xc2,0xeb,0xb4,0xed,0xce,0xf3};
        sprintf(reply_buf, "<script charset='gb2312'>alert('%s');</script>", out);
        strcat(reply_buf, "<script>window.location.href = '/login.html';</script>");
        send(sock,reply_buf,strlen(reply_buf),0);
    }
    return 0;
}

static int handle_add(int sock, const char *input)
{
    int number1 = 0, number2 = 0;
    int n = sscanf(input, "\"data1=%ddata2=%d\"", &number1, &number2);
    printf("num1 = %d\n", number1);

    char reply_buf[HTML_SIZE] = {0};
    if(n != 2)
    {
        const char *err = "参数格式错误，应为 \"data1=1data2=2\"";
        printf("handle_add parse fail, n=%d\n", n);
        send(sock,err,strlen(err),0);
        return 0;
    }

    int sum = number1 + number2;
    sprintf(reply_buf, "%d", sum);
    send(sock,reply_buf,strlen(reply_buf),0);
    return 0;
}

/* ============================================================================
   二、老接口 /api?cmd=xxx（realtime.html 和命令行用的，返回裸JSON，无包装）
   ============================================================================ */
static int handle_realtime_raw(int sock)
{
    char reply[512];
    shm_sensor_data_t *shm_data = NULL;

    if(shm_sensor_attach(&shm_data) != 0)
    {
        const char *err = "{\"error\":\"采集进程未运行(共享内存不存在)\"}";
        send(sock,err,strlen(err),0);
        return 0;
    }

    if(shm_rdlock(shm_data, 300) == 0)
    {
        char timestr[32] = "-";
        time_t ts = shm_data->timestamp;
        if(ts > 0)
        {
            struct tm tmv;
            localtime_r(&ts, &tmv);
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tmv);
        }
        snprintf(reply, sizeof(reply),
            "{\"temp\":%.1f,\"humi\":%.1f,\"status\":%d,\"ts\":%ld,\"time\":\"%s\"}",
            shm_data->temperature, shm_data->humidity,
            shm_data->device_status, (long)ts, timestr);
        shm_unlock(shm_data);
    }
    else
    {
        snprintf(reply, sizeof(reply), "{\"error\":\"读取共享内存超时\"}");
    }

    shm_sensor_detach(shm_data);
    send(sock,reply,strlen(reply),0);
    return 0;
}

static int handle_set_interval_raw(int sock, const char *params)
{
    char reply[256];
    int sec = web_get_int_param(params, "sec=", -1);
    if(sec < 1 || sec > 60)
    {
        const char *err = "{\"error\":\"sec参数必须是1~60的整数\"}";
        send(sock,err,strlen(err),0);
        return 0;
    }

    int ms = sec * 1000;
    mqd_t mqfd = mq_modbus_open(0);
    if(mqfd == (mqd_t)-1)
    {
        const char *err = "{\"error\":\"消息队列打不开，采集进程可能没在运行\"}";
        send(sock,err,strlen(err),0);
        return 0;
    }

    int rc = mq_send_cmd(mqfd, CMD_SET_INTERVAL, ms);
    mq_modbus_close(mqfd);
    printf("send CMD_SET_INTERVAL %d ms, rc=%d\n", ms, rc);

    snprintf(reply,sizeof(reply),
        "{\"result\":\"ok\",\"interval_ms\":%d,\"send_rc\":%d}", ms, rc);
    send(sock,reply,strlen(reply),0);
    return 0;
}

static int handle_history_raw(int sock, const char *params)
{
    int limit = web_get_int_param(params, "limit=", 10);
    if(limit < 1 || limit > 100) limit = 10;

    char json[4096] = {0};
    sqlite3 *db = web_get_db();
    if(db == NULL || db_query_sensor_history(db, json, sizeof(json), limit) != 0)
    {
        const char *err = "{\"error\":\"查询历史失败(sensor.db或sensor_data表不存在)\"}";
        send(sock,err,strlen(err),0);
        return 0;
    }
    send(sock,json,strlen(json),0);
    return 0;
}

/* ============================================================================
   三、总入口：由 thttpd.c 的 handle_request 调用
   ============================================================================ */
int parse_and_process(int sock, const char *url, const char *query_string,
                      const char *input, const char *auth)
{
    // 头文件里说了 GET请求时 input 可能是NULL，这里统一兜一下，避免 strstr(NULL,...) 崩
    if(input == NULL) input = "";
    if(query_string == NULL) query_string = "";
    if(url == NULL) url = "";
    if(auth == NULL) auth = "";

    // ---------- RESTful 接口 /api/xxx ----------
    if(strncmp(url, "/api/", 5) == 0)
    {
        return api_dispatch(sock, url, query_string, input, auth);
    }

    // ---------- 老接口 /api?cmd=xxx（返回裸JSON，realtime.html在用） ----------
    if(strncmp(url, "/api", 4) == 0)
    {
        const char *params = (query_string[0] != '\0') ? query_string : input;
        if(params != NULL && strstr(params, "cmd=") != NULL)
        {
            if(strstr(params, "cmd=realtime")) return handle_realtime_raw(sock);
            if(strstr(params, "cmd=interval")) return handle_set_interval_raw(sock, params);
            if(strstr(params, "cmd=history"))  return handle_history_raw(sock, params);

            const char *err = "{\"error\":\"未知的cmd参数，可用: realtime / interval&sec=N / history&limit=N\"}";
            send(sock,err,strlen(err),0);
            return 0;
        }
    }

    // ---------- 表单业务：登录 / 求和 ----------
    if(strstr(input, "username=") && strstr(input, "password="))
    {
        return handle_login(sock, input);
    }
    if(strstr(input, "data1=") && strstr(input, "data2="))
    {
        return handle_add(sock, input);
    }

    // ---------- 其余：默认JSON ----------
    const char* json_response = "{\"message\": \"Hello, client!\"}";
    send(sock, json_response, strlen(json_response), 0);
    return 0;
}
