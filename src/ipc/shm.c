#include "shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SHM_KEY 0x123456
static int shmid = -1;

/* 拿一次当前时间 + timeout 毫秒后的绝对时间，给 pthread 的 timedlock 用 */
static void make_deadline(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    while(ts->tv_nsec >= 1000000000L)      // tv_nsec 必须小于1秒，否则返回EINVAL
    {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

int shm_sensor_create(shm_sensor_data_t **out_ptr)
{
    if(out_ptr == NULL) return -1;

    shmid = shmget(SHM_KEY, sizeof(shm_sensor_data_t), 0666|IPC_CREAT);
    if(shmid < 0 && errno == EINVAL)
    {
        // 大概率是上一次遗留的老结构体共享内存（大小不一致），删掉重建
        // 这样改了结构体之后，采集进程不用手工清理就能直接起来
        int old = shmget(SHM_KEY, 0, 0666);
        if(old >= 0)
        {
            shmctl(old, IPC_RMID, NULL);
            printf("[shm] 清理了旧结构体的共享内存 id=%d，重新创建\n", old);
        }
        shmid = shmget(SHM_KEY, sizeof(shm_sensor_data_t), 0666|IPC_CREAT);
    }
    if(shmid < 0)
    {
        perror("shmget");
        return -1;
    }

    shm_sensor_data_t *p = (shm_sensor_data_t*)shmat(shmid, NULL, 0);
    if(p == (void*)-1)
    {
        perror("shmat");
        return -1;
    }

    // 进程间共享的读写锁，只能在"创建者"这边初始化一次
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&p->rwlock, &attr);
    pthread_rwlockattr_destroy(&attr);

    memset((char*)p + sizeof(int) + sizeof(pthread_rwlock_t), 0,
           sizeof(shm_sensor_data_t) - sizeof(int) - sizeof(pthread_rwlock_t));
    p->magic = SHM_MAGIC;      // 魔数最后写，写完别人才能安全读

    *out_ptr = p;
    return 0;
}

int shm_sensor_attach(shm_sensor_data_t **out_ptr)
{
    if(out_ptr == NULL) return -1;
    *out_ptr = NULL;

    // 不带 IPC_CREAT：采集进程没跑就不要凭空创建
    int id = shmget(SHM_KEY, sizeof(shm_sensor_data_t), 0666);
    if(id < 0) return -1;

    shm_sensor_data_t *p = (shm_sensor_data_t*)shmat(id, NULL, 0);
    if(p == (void*)-1) return -1;

    if(p->magic != SHM_MAGIC)
    {
        // 老版本共享内存，结构体对不上，宁可报错也别读垃圾
        shmdt(p);
        return -1;
    }

    *out_ptr = p;
    return 0;
}

void shm_sensor_detach(shm_sensor_data_t *p)
{
    if(p) shmdt(p);
}

int shm_sensor_destroy(void)
{
    if(shmid >= 0)   // shmid 从0开始编号，用 >0 判断会漏掉id为0的段
    {
        int rc = shmctl(shmid, IPC_RMID, NULL);
        shmid = -1;
        return rc;
    }
    return 0;
}

int shm_wrlock(shm_sensor_data_t *p, int timeout_ms)
{
    if(p == NULL || p->magic != SHM_MAGIC) return -1;
    struct timespec ts;
    make_deadline(&ts, timeout_ms);
    return pthread_rwlock_timedwrlock(&p->rwlock, &ts);
}

int shm_rdlock(shm_sensor_data_t *p, int timeout_ms)
{
    if(p == NULL || p->magic != SHM_MAGIC) return -1;
    struct timespec ts;
    make_deadline(&ts, timeout_ms);
    return pthread_rwlock_timedrdlock(&p->rwlock, &ts);
}

void shm_unlock(shm_sensor_data_t *p)
{
    if(p) pthread_rwlock_unlock(&p->rwlock);
}
