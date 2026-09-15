#include "http/thttpd.h"
#include <sys/types.h>
#include <stdint.h>
#include <sys/wait.h>

static void* msg_request(void *arg)
{
    // 使用uintptr_t安全转回整数，消除大小不匹配警告
    int sock = (int)(uintptr_t)arg;

    pthread_detach(pthread_self());    
    //handler_msg作为所有的请求处理入口
    handler_msg(sock);
    // 线程返回NULL，不再把int强制转为指针，消除第三个警告
    return NULL;
}

int main(int argc,char* argv[])
{
    //如果不传递端口，那么使用默认端口80
    int port = 80;
    if(argc > 1)
    {
        port = atoi(argv[1]);
    }
    //初始化服务器-----------1
    int lis_sock=init_server(port);
    while(1)
    {
        struct sockaddr_in peer;    // 用来装客户端的 IP 和端口
        socklen_t len=sizeof(peer);
        
        int sock=accept(lis_sock,(struct sockaddr*)&peer,&len);
        
        if(sock<0)
        {
            perror("accept failed");
            continue;
        }
        
        // 用uintptr_t做中转，安全转成void*
        pthread_t tid;
        if(pthread_create(&tid,NULL,msg_request,(void*)(uintptr_t)sock) >0)
        {
            perror("pthread_create failed");
            close(sock);
        }
    }
    return 0;
}
