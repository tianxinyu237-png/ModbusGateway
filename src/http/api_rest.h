/***********************************************************************************
版权信息:        Coffee Tech.
作者:           jiaoyue
日期:           2022-03-24
文件描述:    RESTful 接口层头文件（ModbusGateway 前端用的 /api/xxx 接口）
***********************************************************************************/
#ifndef API_REST_H
#define API_REST_H

#include <sqlite3.h>

/**
 * @brief 取网页侧共用的数据库句柄（第一次调用时打开，并补建 users/sensor_data/logs 表）
 * @return 成功返回句柄，失败返回 NULL
 * @note 用 WAL 模式打开，可以和采集进程同时读写
 */
sqlite3 *web_get_db(void);

/**
 * @brief 从 "hours=24&limit=10" 这种串里取整数参数
 * @param s 参数串，可为 NULL
 * @param key 形如 "hours="
 * @param def 取不到时的默认值
 */
int web_get_int_param(const char *s, const char *key, int def);

/**
 * @brief REST 接口分发入口
 * @param sock 客户端套接字（本函数会自己发送完整HTTP响应，含状态行）
 * @param url 请求路径，形如 /api/realtime
 * @param query_string URL里?后面的参数，可为空串
 * @param input 请求体（前端发的是JSON），可为空串
 * @param auth 请求头 Authorization 的值，形如 "Bearer xxxx"，可为空串
 * @return 0
 * @note 除 /api/register、/api/login 外都需要有效 token，否则回 401
 */
int api_dispatch(int sock, const char *url, const char *query_string,
                 const char *input, const char *auth);

#endif  // API_REST_H
