#ifndef SHM_H
#define SHM_H
#include <sys/types.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>

/* 结构体版本标记：采集进程建段时写入，web进程挂接后先校验，
   避免新旧结构体混用读到垃圾数据 */
#define SHM_MAGIC 0x53454E53        /* "SENS" */

typedef struct{
    int magic;                      // 必须等于 SHM_MAGIC
    pthread_rwlock_t rwlock;        // 进程间读写锁：采集进程持写锁，web进程持读锁
    float temperature;
    float humidity;
    float temp_max;                 // 本次运行最高温度
    float temp_min;                 // 本次运行最低温度
    unsigned int sample_count;      // 本次运行已采集点数
    time_t timestamp;               // 最近一次采集时刻
    time_t start_time;              // 采集进程启动时刻（算uptime用）
    int device_status;              // 0离线 1在线 2故障(温度超阈值)
    int slave_id;                   // Modbus 从机地址
    int period_ms;                  // 当前采集周期
    float threshold;                // 温度告警阈值（℃）
    unsigned short last_regs[8];    // 最近一次读到的寄存器原始值（readRegister用）
    int last_regs_addr;             // 上面这批寄存器的起始地址
    int last_regs_count;            // 上面这批寄存器的个数

    /* 数据来源标记：让网页和日志能明确显示"这是模拟数据还是真实设备"，
       答辩/验收时不会含糊 */
    char source_name[48];           // 来源名字，如 modbus-simulator / 车间1号温湿度计
    int  source_simulated;          // 1=模拟数据  0=真实设备
    char device_target[64];         // 实际连接目标，如 tcp 192.168.1.100:502 从机1
}shm_sensor_data_t;

int shm_sensor_create(shm_sensor_data_t **out_ptr);
// 只挂接已经存在的共享内存（读端用，比如web进程读实时值），不存在或版本不符返回-1
int shm_sensor_attach(shm_sensor_data_t **out_ptr);
void shm_sensor_detach(shm_sensor_data_t *p);
int shm_sensor_destroy(void);

// 读写锁封装：带超时，拿不到锁返回-1（绝不永久阻塞）
int shm_wrlock(shm_sensor_data_t *p, int timeout_ms);
int shm_rdlock(shm_sensor_data_t *p, int timeout_ms);
void shm_unlock(shm_sensor_data_t *p);

#endif
