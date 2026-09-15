/***********************************************************************************
版权信息:        Coffee Tech.
作者:           jiaoyue
日期:           2022-03-24
文件描述:    http请求自定义业务处理头文件，声明业务处理接口
***********************************************************************************/
#ifndef CUSTOM_HANDLE_H
#define CUSTOM_HANDLE_H

#include <stdio.h>
#include <string.h>

/**
 * @brief 解析HTTP请求参数，并执行自定义业务逻辑
 * @param sock 客户端连接套接字，用于向浏览器返回响应数据
 * @param url 请求的资源路径（不含wwwroot前缀），如 /login、/api/realtime
 * @param query_string GET请求URL中?后面的查询参数；无参数则为NULL或空串
 * @param input POST请求的请求体数据；POST无数据/GET请求时可为NULL
 * @param auth 请求头 Authorization 的值，形如 "Bearer xxxxx"，没有则为空串
 * @return 业务处理返回值（可自行约定，0代表成功）
 * @note REST接口(/api/xxx)由本函数自己发送完整HTTP响应（含状态行），
 *       所以 thttpd.c 对 /api/ 开头的请求不会再预先发 200 状态行
 */
int parse_and_process(int sock, const char *url, const char *query_string,
                      const char *input, const char *auth);

#endif  // CUSTOM_HANDLE_H
