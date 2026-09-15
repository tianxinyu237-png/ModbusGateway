#include "mq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

mqd_t mq_modbus_open(int create)
{
    mqd_t mqfd;
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(mq_msg_t);
    attr.mq_curmsgs =0;

    if(create)
    {
        mqfd = mq_open(MQ_NAME, O_RDWR|O_CREAT, 0666, &attr);
    }
    else
    {
        mqfd = mq_open(MQ_NAME, O_RDWR);
    }
    if(mqfd == (mqd_t)-1)
    {
        perror("mq_open");
    }
    return mqfd;
}

int mq_send_cmd(mqd_t mqfd,int cmd,int param)
{
    mq_msg_t msg;
    memset(&msg,0,sizeof(msg));
    msg.cmd = cmd;
    msg.param = param;
    return mq_send(mqfd, (char*)&msg, sizeof(msg), 0);
}

int mq_recv_msg(mqd_t mqfd,mq_msg_t *msg,int timeout_ms)
{
    struct timespec ts;
    memset(msg,0,sizeof(mq_msg_t));
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms /1000;
    ts.tv_nsec += (timeout_ms%1000)*1000000L;
    // tv_nsec 必须小于1秒，否则 mq_timedreceive 直接返回 EINVAL
    while(ts.tv_nsec >= 1000000000L)
    {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }

    ssize_t ret = mq_timedreceive(mqfd, (char*)msg, sizeof(mq_msg_t), NULL, &ts);
    if(ret <0)
    {
        if(errno == ETIMEDOUT) return -2;
        return -1;
    }
    return 0;
}

void mq_modbus_close(mqd_t fd)
{
    if(fd != (mqd_t)-1) mq_close(fd);
}
void mq_modbus_unlink(void)
{
    mq_unlink(MQ_NAME);
}
