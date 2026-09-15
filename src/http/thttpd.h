#ifndef __THTTPD_H__    // 头文件保护，防止重复 include
#define __THTTPD_H__

#include <stdio.h>         // printf / perror / sprintf
#include <string.h>         // strcmp / strlen / strcat
#include <strings.h>        // ★ strcasecmp / strncasecmp（POSIX，MSVC 没有）
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>


#define SIZE 4096

int init_server(int _port);
int handler_msg(int sock);

#endif
