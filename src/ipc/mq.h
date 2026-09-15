#ifndef MQ_H
#define MQ_H
#include <mqueue.h>

/* 消息队列里的指令码 —— web进程下发，采集进程消费
   参数的约定（param 是个int，需要传小数时放大成整数）：
     CMD_QUERY         : 高16位=寄存器起始地址, 低16位=读取个数
     CMD_SET_INTERVAL  : 采集周期，单位毫秒
     CMD_SET_THRESHOLD : 温度告警阈值 = param/100.0 ℃（3250 -> 32.5℃）
     CMD_RESTART       : 无参数
*/
#define CMD_QUERY         1
#define CMD_SET_INTERVAL  2
#define CMD_SET_THRESHOLD 3
#define CMD_RESTART       4

typedef struct{
    int cmd;
    int param;
    char text[64];
}mq_msg_t;

#define MQ_NAME "/modbus_sensor_mq"

mqd_t mq_modbus_open(int create);
int mq_send_cmd(mqd_t mqfd,int cmd,int param);
int mq_recv_msg(mqd_t mqfd,mq_msg_t *msg,int timeout_ms);
void mq_modbus_close(mqd_t fd);
void mq_modbus_unlink(void);

#endif
