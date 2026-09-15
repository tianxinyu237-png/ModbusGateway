/***********************************************************************************
 采集进程（Modbus TCP/RTU 温湿度采集 + 共享内存 + 消息队列 + SQLite + 分级日志）
 ---------------------------------------------------------------------------
 运行方式：
   ./collector.out                 前台运行（调试用，Ctrl+C 退出）
   ./collector.out --daemon        后台守护运行（fork+setsid，子进程异常退出自动拉起）
   ./collector.out --stop          停止守护进程（读 run/collector.pid）
   ./collector.out --status        查看运行状态（pid + 共享内存实时数据 + 数据来源）
   ./collector.out --show-conf     打印当前生效的配置（含配置文件和命令行覆盖后的结果）
   ./collector.out --help          帮助

 设备配置：
   默认读同目录下的 collector.conf（key=value，# 开头是注释），不存在就用内置默认值。
   命令行参数会覆盖配置文件，例如接真实设备不用改代码、不用重新编译：
     ./collector.out --host 192.168.1.100 --port 502 --slave 1 --real --source "车间1号温湿度计"
   串口设备（RS485/Modbus RTU）：
     ./collector.out --transport rtu --serial /dev/ttyUSB0 --baud 9600

 守护进程结构（架构图里的"守护进程 + 异常自愈"）：
   父进程(daemon/supervisor) : fork+setsid 脱离终端、写pid文件、负责监管
        └── 子进程(worker)    : 真正的采集循环（连设备、写共享内存、落库）
   子进程异常退出(崩溃/被kill -9)时，父进程记日志并在3秒后重新拉起，最多MAX_RESTARTS次
   子进程正常退出(收到信号收尾)时，父进程跟着退出

 日志：同时写 SQLite logs 表 和 syslog（IDENT=modbus-collector）
 ***********************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <modbus.h>          // 本机 libmodbus 头文件装在 /usr/include/modbus.h
#include "src/ipc/shm.h"
#include "src/ipc/mq.h"
#include "src/db/db.h"

#define SYSLOG_IDENT      "modbus-collector"
#define CONF_FILE         "collector.conf"   // 默认配置文件（相对启动目录）
#define MAX_RESTARTS      20                 // 守护进程最多拉起子进程的次数，防无限重启
#define RESTART_DELAY_SEC 3                  // 子进程异常退出后等几秒再拉起
#define MODBUS_RECONNECT_AFTER 5             // 连续读失败几次就尝试重连设备

/* ============================ 设备/采集配置 ============================ */
typedef struct{
    /* 设备连接 */
    char  transport[8];     // tcp / rtu
    char  host[64];         // tcp: 设备IP
    int   port;             // tcp: 端口（标准Modbus TCP是502）
    int   slave;            // 从机地址
    char  serial[64];       // rtu: 串口设备节点
    int   baud;             // rtu: 波特率
    char  parity;           // rtu: 校验位 N/E/O
    int   databits;         // rtu: 数据位
    int   stopbits;         // rtu: 停止位
    /* 寄存器映射：设备不同就改这里，不用改代码 */
    int   reg_temp;         // 温度寄存器地址
    int   reg_humi;         // 湿度寄存器地址
    int   reg_base;         // 一次连续读的起始地址（取 reg_temp/reg_humi 里较小的那个）
    int   reg_count;        // 一次连续读几个寄存器
    float scale_temp;       // 温度 = 原始寄存器值 / scale_temp
    float scale_humi;       // 湿度 = 原始寄存器值 / scale_humi
    /* 采集参数 */
    int   interval_ms;      // 采集周期
    float threshold;        // 温度告警阈值 ℃
    /* 数据来源标记（只影响显示，不改数据） */
    char  source_name[48];  // 来源名字
    int   source_simulated; // 1=模拟数据 0=真实设备
}dev_cfg_t;

static dev_cfg_t g_cfg;

/* ---------------- 全局运行状态 ---------------- */
static int      g_running = 1;
static sqlite3 *g_db      = NULL;
static char     g_db_path[PATH_MAX];
static char     g_log_path[PATH_MAX];
static char     g_pid_path[PATH_MAX];
static char     g_conf_path[PATH_MAX];
static volatile sig_atomic_t g_super_stop = 0;
static volatile pid_t        g_child_pid  = -1;

/* ============================ 配置解析 ============================ */
static void cfg_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.transport, sizeof(g_cfg.transport), "tcp");
    snprintf(g_cfg.host,      sizeof(g_cfg.host),      "127.0.0.1");
    g_cfg.port        = 5020;          // 本机模拟器端口；真实设备一般是 502
    g_cfg.slave       = 1;
    snprintf(g_cfg.serial,    sizeof(g_cfg.serial),    "/dev/ttyUSB0");
    g_cfg.baud        = 9600;
    g_cfg.parity      = 'N';
    g_cfg.databits    = 8;
    g_cfg.stopbits    = 1;
    g_cfg.reg_temp    = 0;
    g_cfg.reg_humi    = 1;
    g_cfg.reg_base    = 0;
    g_cfg.reg_count   = 2;
    g_cfg.scale_temp  = 10.0f;         // 寄存器 265 -> 26.5℃
    g_cfg.scale_humi  = 10.0f;
    g_cfg.interval_ms = 2000;
    g_cfg.threshold   = 32.0f;
    /* 默认连的是本机模拟器，所以默认就标成"模拟数据"，不糊弄人 */
    snprintf(g_cfg.source_name, sizeof(g_cfg.source_name), "modbus-simulator");
    g_cfg.source_simulated = 1;
}

static void cfg_auto_base(void)
{
    // 一次读多个寄存器时，从两个地址里较小的那个开始连续读
    int lo = (g_cfg.reg_temp < g_cfg.reg_humi) ? g_cfg.reg_temp : g_cfg.reg_humi;
    int hi = (g_cfg.reg_temp > g_cfg.reg_humi) ? g_cfg.reg_temp : g_cfg.reg_humi;
    if(g_cfg.reg_base == 0 && g_cfg.reg_count == 2 && lo != 0) g_cfg.reg_base = lo;
    if(g_cfg.reg_count < (hi - g_cfg.reg_base + 1)) g_cfg.reg_count = hi - g_cfg.reg_base + 1;
    if(g_cfg.reg_count < 1) g_cfg.reg_count = 1;
    if(g_cfg.reg_count > 8) g_cfg.reg_count = 8;
}

static int cfg_apply_kv(const char *key, const char *val)
{
    if(!strcmp(key,"transport"))     { snprintf(g_cfg.transport,sizeof(g_cfg.transport),"%s",val); }
    else if(!strcmp(key,"host"))     { snprintf(g_cfg.host,sizeof(g_cfg.host),"%s",val); }
    else if(!strcmp(key,"port"))     { g_cfg.port = atoi(val); }
    else if(!strcmp(key,"slave"))    { g_cfg.slave = atoi(val); }
    else if(!strcmp(key,"serial"))   { snprintf(g_cfg.serial,sizeof(g_cfg.serial),"%s",val); }
    else if(!strcmp(key,"baud"))     { g_cfg.baud = atoi(val); }
    else if(!strcmp(key,"parity"))   { g_cfg.parity = toupper((unsigned char)val[0]); }
    else if(!strcmp(key,"databits")) { g_cfg.databits = atoi(val); }
    else if(!strcmp(key,"stopbits")) { g_cfg.stopbits = atoi(val); }
    else if(!strcmp(key,"reg_temp")) { g_cfg.reg_temp = atoi(val); }
    else if(!strcmp(key,"reg_humi")) { g_cfg.reg_humi = atoi(val); }
    else if(!strcmp(key,"reg_base")) { g_cfg.reg_base = atoi(val); }
    else if(!strcmp(key,"reg_count")){ g_cfg.reg_count = atoi(val); }
    else if(!strcmp(key,"scale_temp")){ g_cfg.scale_temp = atof(val); }
    else if(!strcmp(key,"scale_humi")){ g_cfg.scale_humi = atof(val); }
    else if(!strcmp(key,"interval_ms")){ g_cfg.interval_ms = atoi(val); }
    else if(!strcmp(key,"interval")) { g_cfg.interval_ms = atoi(val); }
    else if(!strcmp(key,"threshold")){ g_cfg.threshold = atof(val); }
    else if(!strcmp(key,"source_name")){ snprintf(g_cfg.source_name,sizeof(g_cfg.source_name),"%s",val); }
    else if(!strcmp(key,"source_simulated")){ g_cfg.source_simulated = atoi(val) ? 1 : 0; }
    else return -1;              // 不认识的键：返回-1，由调用方提示
    return 0;
}

static int cfg_load_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    int ok = 0;

    if(fp == NULL) return -1;

    while(fgets(line, sizeof(line), fp) != NULL)
    {
        char *p = line, *eq, *key, *val;
        while(*p == ' ' || *p == '\t') p++;
        if(*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;   // 注释/空行

        eq = strchr(p, '=');
        if(eq == NULL) continue;
        *eq = '\0';
        key = p;
        val = eq + 1;
        // 去掉行内注释：值里出现 '#' 就截断（配置文件里 '#' 只能当注释用，
        // 否则 host = 127.0.0.1   # 注释  会被整串当成IP，踩过这个坑）
        { char *hash = strchr(val, '#'); if(hash != NULL) *hash = '\0'; }
        // 去掉两边空白和行尾换行
        { char *e = key + strlen(key); while(e>key && (e[-1]==' '||e[-1]=='\t')) *--e='\0'; }
        while(*val == ' ' || *val == '\t') val++;
        { char *e = val + strlen(val); while(e>val && (e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '||e[-1]=='\t')) *--e='\0'; }

        if(cfg_apply_kv(key, val) != 0)
            fprintf(stderr, "[配置] 警告: %s 里的 '%s' 不是可识别的配置项，已忽略\n", path, key);
        else
            ok++;
    }
    fclose(fp);
    printf("[配置] 已加载 %s（%d 项）\n", path, ok);
    return 0;
}

static void cfg_print(void)
{
    printf("===== 当前生效的采集配置 =====\n");
    printf("  配置文件      : %s%s\n", g_conf_path[0] ? g_conf_path : "(未找到，用内置默认值)",
           g_conf_path[0] ? "" : "");
    if(strcmp(g_cfg.transport, "rtu") == 0)
        printf("  设备连接      : RTU 串口 %s  %d,%c,%d,%d\n",
               g_cfg.serial, g_cfg.baud, g_cfg.parity, g_cfg.databits, g_cfg.stopbits);
    else
        printf("  设备连接      : TCP %s:%d\n", g_cfg.host, g_cfg.port);
    printf("  从机地址      : %d\n", g_cfg.slave);
    printf("  寄存器映射    : 温度=寄存器%d  湿度=寄存器%d  (连续读 %d 个，从 %d 开始)\n",
           g_cfg.reg_temp, g_cfg.reg_humi, g_cfg.reg_count, g_cfg.reg_base);
    printf("  量纲换算      : 温度=原始值/%.1f   湿度=原始值/%.1f\n", g_cfg.scale_temp, g_cfg.scale_humi);
    printf("  采集周期      : %d ms\n", g_cfg.interval_ms);
    printf("  告警阈值      : %.1f ℃\n", g_cfg.threshold);
    printf("  数据来源      : %s —— %s\n", g_cfg.source_name,
           g_cfg.source_simulated ? "模拟数据(测试用)" : "真实设备");
    printf("==============================\n");
}

/* ============================ 日志 ============================ */
static void log_msg(const char *level, const char *module, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    int prio = LOG_INFO;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if(level == NULL) level = "INFO";
    if(strcmp(level, "ERROR") == 0)      prio = LOG_ERR;
    else if(strcmp(level, "WARN") == 0)  prio = LOG_WARNING;
    else if(strcmp(level, "DEBUG") == 0) prio = LOG_DEBUG;

    if(g_db != NULL) db_log(g_db, level, module, "%s", msg);
    syslog(prio, "[%s] %s", module ? module : "-", msg);

    printf("[%s][%s] %s\n", level, module ? module : "-", msg);
    fflush(stdout);
}

/* ============================ 信号 ============================ */
static void worker_signal_handler(int sig) { (void)sig; g_running = 0; }

static void super_signal_handler(int sig)
{
    (void)sig;
    g_super_stop = 1;
    if(g_child_pid > 0) kill(g_child_pid, SIGTERM);
}

/* ============================ pid 文件 ============================ */
static int write_pid_file(pid_t pid)
{
    FILE *fp = fopen(g_pid_path, "w");
    if(fp == NULL) return -1;
    fprintf(fp, "%d\n", (int)pid);
    fclose(fp);
    return 0;
}

static pid_t read_pid_file(void)
{
    FILE *fp = fopen(g_pid_path, "r");
    int pid = 0;
    if(fp == NULL) return -1;
    if(fscanf(fp, "%d", &pid) != 1) pid = 0;
    fclose(fp);
    return (pid > 0) ? (pid_t)pid : -1;
}

static int process_alive(pid_t pid)
{
    if(pid <= 0) return 0;
    return (kill(pid, 0) == 0) ? 1 : 0;
}

/* ============================ 守护化 ============================ */
static int daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if(pid < 0) return -1;
    if(pid > 0)
    {
        printf("采集守护进程已在后台启动，pid=%d（日志: %s）\n", (int)pid, g_log_path);
        printf("查看状态: ./collector.out --status    停止: ./collector.out --stop\n");
        exit(0);
    }

    if(setsid() < 0) return -1;

    pid = fork();
    if(pid < 0) return -1;
    if(pid > 0) exit(0);

    umask(0);
    if(chdir("/") != 0) return -1;      // 守护进程惯例：切到 /（下面全用绝对路径）

    fd = open(g_log_path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if(fd >= 0)
    {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if(fd > STDERR_FILENO) close(fd);
    }
    fd = open("/dev/null", O_RDONLY);
    if(fd >= 0)
    {
        dup2(fd, STDIN_FILENO);
        if(fd > STDIN_FILENO) close(fd);
    }

    write_pid_file(getpid());
    return 0;
}

/* ============================ worker：真正干活 ============================ */
static int worker_main(void)
{
    modbus_t *ctx = NULL;
    shm_sensor_data_t *shm_data = NULL;
    mqd_t mq_fd;
    unsigned short reg_buf[8];
    time_t last_modbus_err_log = 0;
    int was_over_threshold = 0;
    int modbus_err_count = 0;
    int iv = 0, ih = 0;               // 温度/湿度在 reg_buf 里的下标
    char target[128];

    signal(SIGINT,  worker_signal_handler);
    signal(SIGTERM, worker_signal_handler);

    cfg_auto_base();
    iv = g_cfg.reg_temp - g_cfg.reg_base;
    ih = g_cfg.reg_humi - g_cfg.reg_base;
    if(iv < 0 || iv >= g_cfg.reg_count) iv = 0;      // 配错也不至于读越界
    if(ih < 0 || ih >= g_cfg.reg_count) ih = (g_cfg.reg_count > 1) ? 1 : 0;

    if(strcmp(g_cfg.transport, "rtu") == 0)
        snprintf(target, sizeof(target), "rtu %s %d,%c,%d,%d 从机%d",
                 g_cfg.serial, g_cfg.baud, g_cfg.parity, g_cfg.databits, g_cfg.stopbits, g_cfg.slave);
    else
        snprintf(target, sizeof(target), "tcp %s:%d 从机%d", g_cfg.host, g_cfg.port, g_cfg.slave);

    // ========= 1、共享内存 + 消息队列 =========
    if(shm_sensor_create(&shm_data) < 0)
    {
        log_msg("ERROR", "shm", "共享内存创建失败: %s", strerror(errno));
        return 1;
    }
    mq_fd = mq_modbus_open(1);
    if(mq_fd == (mqd_t)-1)
    {
        log_msg("ERROR", "mq", "消息队列打开失败: %s", strerror(errno));
        shm_sensor_detach(shm_data);
        return 1;
    }

    // ========= 2、SQLite =========
    if(db_open(g_db_path, &g_db) != 0)
    {
        log_msg("ERROR", "db", "数据库打开失败: %s", g_db_path);
        mq_modbus_close(mq_fd);
        shm_sensor_detach(shm_data);
        return 1;
    }
    db_init_runtime(g_db);

    if(shm_wrlock(shm_data, 1000) == 0)
    {
        shm_data->start_time       = time(NULL);
        shm_data->slave_id         = g_cfg.slave;
        shm_data->period_ms        = g_cfg.interval_ms;
        shm_data->threshold        = g_cfg.threshold;
        shm_data->device_status    = 0;
        snprintf(shm_data->source_name,     sizeof(shm_data->source_name),     "%s", g_cfg.source_name);
        snprintf(shm_data->device_target,   sizeof(shm_data->device_target),   "%s", target);
        shm_data->source_simulated = g_cfg.source_simulated ? 1 : 0;
        shm_unlock(shm_data);
    }

    // ========= 3、连接设备 =========
    if(strcmp(g_cfg.transport, "rtu") == 0)
    {
        ctx = modbus_new_rtu(g_cfg.serial, g_cfg.baud, g_cfg.parity, g_cfg.databits, g_cfg.stopbits);
        if(ctx == NULL) log_msg("ERROR", "modbus", "modbus_new_rtu(%s) 创建失败", g_cfg.serial);
    }
    else
    {
        ctx = modbus_new_tcp(g_cfg.host, g_cfg.port);
        if(ctx == NULL) log_msg("ERROR", "modbus", "modbus_new_tcp(%s:%d) 创建失败", g_cfg.host, g_cfg.port);
    }

    if(ctx == NULL || modbus_set_slave(ctx, g_cfg.slave) == -1 || modbus_connect(ctx) == -1)
    {
        log_msg("ERROR", "modbus", "连接设备失败(%s): %s", target, modbus_strerror(errno));
        log_msg("ERROR", "modbus", "排查建议: 1)设备IP/端口是否写对(标准Modbus TCP端口是502)  "
                                   "2)设备是否上电且在网(ping一下)  3)从机地址(站号)是否正确  "
                                   "4)连本机模拟器时要先起 python3 tools/modbus_slave.py  "
                                   "5)端口通不通: nc -vz <设备IP> <端口>");
        if(ctx) modbus_free(ctx);
        db_close(g_db); g_db = NULL;
        mq_modbus_close(mq_fd);
        shm_sensor_detach(shm_data);
        shm_sensor_destroy();
        return 1;
    }

    log_msg("INFO", "collect", "采集进程启动(pid=%d)，设备连接成功: %s", (int)getpid(), target);
    log_msg("INFO", "collect", "寄存器映射: 温度=寄存器%d/%s  湿度=寄存器%d/%s（连续读%d个，起始%d）",
            g_cfg.reg_temp, "÷scale", g_cfg.reg_humi, "÷scale", g_cfg.reg_count, g_cfg.reg_base);
    log_msg("INFO", "collect", "数据来源: %s —— %s", g_cfg.source_name,
            g_cfg.source_simulated ? "模拟数据(测试用，不是真实传感器)" : "真实设备");
    if(g_cfg.source_simulated)
        log_msg("WARN", "collect", "当前为模拟数据源：如需接真实设备，改 collector.conf 或加 --host/--real 参数");

    // ========= 4、主循环 =========
    while(g_running)
    {
        mq_msg_t cmd_msg;
        int ret = mq_recv_msg(mq_fd, &cmd_msg, 10);
        if(ret == 0)
        {
            printf("收到web下发指令 cmd=%d, param=%d\n", cmd_msg.cmd, cmd_msg.param);

            if(cmd_msg.cmd == CMD_SET_INTERVAL)
            {
                int interval = cmd_msg.param;
                if(interval >= 100 && interval <= 60000)
                {
                    g_cfg.interval_ms = interval;
                    if(shm_wrlock(shm_data, 1000) == 0)
                    {
                        shm_data->period_ms = g_cfg.interval_ms;
                        shm_unlock(shm_data);
                    }
                    log_msg("INFO", "mq", "采集周期已修改为 %d ms", g_cfg.interval_ms);
                }
                else log_msg("WARN", "mq", "采集周期参数不合法，已忽略: %d", interval);
            }
            else if(cmd_msg.cmd == CMD_SET_THRESHOLD)
            {
                float th = cmd_msg.param / 100.0f;
                if(th > 0.0f && th < 200.0f)
                {
                    g_cfg.threshold = th;
                    if(shm_wrlock(shm_data, 1000) == 0)
                    {
                        shm_data->threshold = g_cfg.threshold;
                        shm_unlock(shm_data);
                    }
                    log_msg("INFO", "mq", "温度告警阈值已修改为 %.1f ℃", g_cfg.threshold);
                }
            }
            else if(cmd_msg.cmd == CMD_QUERY)
            {
                int qaddr = (cmd_msg.param >> 16) & 0xFFFF;
                int qcnt  = cmd_msg.param & 0xFFFF;
                unsigned short tmp[8];
                if(qcnt < 1) qcnt = 1;
                if(qcnt > 8) qcnt = 8;

                int rc2 = modbus_read_registers(ctx, qaddr, qcnt, tmp);
                if(rc2 == qcnt && shm_wrlock(shm_data, 1000) == 0)
                {
                    int k;
                    for(k=0;k<qcnt;k++) shm_data->last_regs[k] = tmp[k];
                    shm_data->last_regs_addr  = qaddr;
                    shm_data->last_regs_count = qcnt;
                    shm_unlock(shm_data);
                    log_msg("DEBUG", "modbus", "按需读取寄存器 addr=%d count=%d 成功", qaddr, qcnt);
                }
                else log_msg("WARN", "modbus", "按需读取寄存器失败 addr=%d count=%d rc=%d", qaddr, qcnt, rc2);
            }
        }

        // ========= 读设备 =========
        int rc = modbus_read_registers(ctx, g_cfg.reg_base, g_cfg.reg_count, reg_buf);
        if(rc == -1)
        {
            modbus_err_count++;
            if(shm_wrlock(shm_data, 1000) == 0)
            {
                shm_data->device_status = 0;
                shm_unlock(shm_data);
            }
            if(time(NULL) - last_modbus_err_log >= 30)
            {
                log_msg("WARN", "modbus", "读取寄存器失败: %s", modbus_strerror(errno));
                last_modbus_err_log = time(NULL);
            }

            // 【异常自愈】连续失败到一定次数就断开重连
            if(modbus_err_count >= MODBUS_RECONNECT_AFTER)
            {
                log_msg("WARN", "modbus", "连续%d次读取失败，尝试重连设备…", modbus_err_count);
                modbus_close(ctx);
                sleep(1);
                if(modbus_connect(ctx) == 0)
                {
                    modbus_err_count = 0;
                    log_msg("INFO", "modbus", "重连成功，恢复采集");
                }
                else
                {
                    log_msg("ERROR", "modbus", "重连失败: %s，2秒后再试", modbus_strerror(errno));
                    sleep(2);
                }
            }
            usleep(500000);
            continue;
        }
        modbus_err_count = 0;

        // 按配置换算（寄存器原始值 / 量纲）
        float temp = reg_buf[iv] / g_cfg.scale_temp;
        float humi = reg_buf[ih] / g_cfg.scale_humi;

        int status = (temp > g_cfg.threshold) ? 2 : 1;
        if(status == 2 && !was_over_threshold)
        {
            log_msg("WARN", "collect", "温度超阈值: %.1f℃ > %.1f℃", temp, g_cfg.threshold);
            was_over_threshold = 1;
        }
        else if(status == 1 && was_over_threshold)
        {
            log_msg("INFO", "collect", "温度已回落到阈值内: %.1f℃", temp);
            was_over_threshold = 0;
        }

        if(shm_wrlock(shm_data, 1000) == 0)
        {
            if(shm_data->sample_count == 0)
            {
                shm_data->temp_max = temp;
                shm_data->temp_min = temp;
            }
            if(temp > shm_data->temp_max) shm_data->temp_max = temp;
            if(temp < shm_data->temp_min) shm_data->temp_min = temp;

            shm_data->temperature    = temp;
            shm_data->humidity       = humi;
            shm_data->device_status  = status;
            shm_data->timestamp      = time(NULL);
            shm_data->sample_count++;
            shm_data->period_ms      = g_cfg.interval_ms;
            shm_data->threshold      = g_cfg.threshold;
            shm_data->slave_id       = g_cfg.slave;
            { int k; for(k=0;k<g_cfg.reg_count && k<8;k++) shm_data->last_regs[k] = reg_buf[k]; }
            shm_data->last_regs_addr  = g_cfg.reg_base;
            shm_data->last_regs_count = g_cfg.reg_count;
            shm_unlock(shm_data);
        }

        printf("采集：温度=%.2f ℃, 湿度=%.2f %%RH\n", temp, humi);

        if(db_insert_sensor_record(g_db, temp, humi, status) != 0)
            log_msg("ERROR", "db", "入库失败，检查 sensor.db 里 sensor_data 表是否存在");

        usleep(g_cfg.interval_ms * 1000);
    }

    printf("\n收到退出信号，准备关闭采集进程\n");
    log_msg("INFO", "collect", "采集进程正常退出");

    if(ctx){ modbus_close(ctx); modbus_free(ctx); }
    if(g_db){ db_close(g_db); g_db = NULL; }
    mq_modbus_close(mq_fd);
    mq_modbus_unlink();
    shm_sensor_detach(shm_data);
    shm_sensor_destroy();

    printf("采集进程安全退出\n");
    return 0;
}

/* ============================ 父进程：监管 ============================ */
static int supervise_loop(void)
{
    int restarts = 0;

    signal(SIGTERM, super_signal_handler);
    signal(SIGINT,  super_signal_handler);

    log_msg("INFO", "daemon", "守护进程启动(pid=%d)，开始监管采集子进程", (int)getpid());
    syslog(LOG_INFO, "daemon started, pid=%d", (int)getpid());

    while(!g_super_stop)
    {
        int status = 0;
        pid_t pid = fork();

        if(pid < 0)
        {
            log_msg("ERROR", "daemon", "fork 失败: %s", strerror(errno));
            sleep(RESTART_DELAY_SEC);
            continue;
        }

        if(pid == 0)
        {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT,  SIG_DFL);
            _exit(worker_main());
        }

        g_child_pid = pid;
        log_msg("INFO", "daemon", "已拉起采集子进程 pid=%d", (int)pid);

        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* retry */ }
        g_child_pid = -1;

        if(g_super_stop)
        {
            log_msg("INFO", "daemon", "收到停止信号，守护进程退出");
            break;
        }
        if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            log_msg("INFO", "daemon", "采集子进程正常退出，守护进程一起退出");
            break;
        }

        restarts++;
        if(WIFSIGNALED(status))
            log_msg("ERROR", "daemon", "采集子进程被信号 %d 终止（第%d次异常），%d秒后重新拉起",
                    WTERMSIG(status), restarts, RESTART_DELAY_SEC);
        else
            log_msg("ERROR", "daemon", "采集子进程异常退出 code=%d（第%d次），%d秒后重新拉起",
                    WEXITSTATUS(status), restarts, RESTART_DELAY_SEC);

        if(restarts >= MAX_RESTARTS)
        {
            log_msg("ERROR", "daemon", "已连续重启 %d 次，放弃拉起（请检查设备/数据库），守护进程退出", restarts);
            break;
        }
        sleep(RESTART_DELAY_SEC);
    }

    if(g_child_pid > 0) kill(g_child_pid, SIGTERM);
    unlink(g_pid_path);
    log_msg("INFO", "daemon", "守护进程已退出");
    return 0;
}

/* ============================ 命令行 ============================ */
static void prepare_paths(void)
{
    char cwd[PATH_MAX];
    if(getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';

    snprintf(g_db_path,   sizeof(g_db_path),   "%s/sensor.db", cwd[0] ? cwd : ".");
    snprintf(g_pid_path,  sizeof(g_pid_path),  "%s/run/collector.pid", cwd[0] ? cwd : ".");
    snprintf(g_conf_path, sizeof(g_conf_path), "%s/" CONF_FILE, cwd[0] ? cwd : ".");

    char run_dir[PATH_MAX], log_dir[PATH_MAX];
    snprintf(run_dir, sizeof(run_dir), "%s/run",  cwd[0] ? cwd : ".");
    snprintf(log_dir, sizeof(log_dir), "%s/logs", cwd[0] ? cwd : ".");
    mkdir(run_dir, 0755);
    mkdir(log_dir, 0755);
    snprintf(g_log_path, sizeof(g_log_path), "%s/collector.log", log_dir);
}

static void do_stop(void)
{
    pid_t pid = read_pid_file();
    int i;

    if(pid < 0 || !process_alive(pid))
    {
        printf("采集守护进程没有在运行（pid文件: %s）\n", g_pid_path);
        unlink(g_pid_path);
        return;
    }

    printf("正在停止采集守护进程 pid=%d …\n", (int)pid);
    kill(-pid, SIGTERM);
    kill(pid,  SIGTERM);

    for(i=0;i<30;i++)
    {
        if(!process_alive(pid)) { printf("已停止\n"); unlink(g_pid_path); return; }
        usleep(200000);
    }
    printf("6秒还没退出，强制结束\n");
    kill(-pid, SIGKILL);
    kill(pid,  SIGKILL);
    unlink(g_pid_path);
}

static void do_status(void)
{
    pid_t pid = read_pid_file();
    shm_sensor_data_t *shm = NULL;

    printf("===== 采集进程状态 =====\n");
    if(pid > 0 && process_alive(pid))
        printf("  守护进程 : 运行中 (pid=%d)\n", (int)pid);
    else
        printf("  守护进程 : 未运行（pid文件: %s）\n", g_pid_path);
    printf("  pid文件  : %s\n", g_pid_path);
    printf("  日志文件 : %s\n", g_log_path);

    if(shm_sensor_attach(&shm) == 0)
    {
        if(shm_rdlock(shm, 300) == 0)
        {
            time_t ts = shm->timestamp;
            char buf[32] = "-";
            if(ts > 0) { struct tm tmv; localtime_r(&ts,&tmv); strftime(buf,sizeof(buf),"%H:%M:%S",&tmv); }
            printf("  共享内存 : 已连接\n");
            printf("  数据来源 : %s  【%s】\n", shm->source_name,
                   shm->source_simulated ? "模拟数据(测试用)" : "真实设备");
            printf("  设备目标 : %s\n", shm->device_target);
            printf("  实时数据 : 温度 %.2f℃  湿度 %.2f%%RH  状态 %s\n",
                   shm->temperature, shm->humidity,
                   shm->device_status==1 ? "在线" : (shm->device_status==0 ? "离线" : "故障(超阈值)"));
            printf("  本次运行 : 最高 %.2f℃ / 最低 %.2f℃ / 已采集 %u 点 / 周期 %dms\n",
                   shm->temp_max, shm->temp_min, shm->sample_count, shm->period_ms);
            printf("  最近采集 : %s（阈值 %.1f℃）\n", buf, shm->threshold);
            shm_unlock(shm);
        }
        shm_sensor_detach(shm);
    }
    else
    {
        printf("  共享内存 : 不存在（采集子进程没跑或已退出）\n");
    }
}

static void usage(const char *prog)
{
    printf("用法: %s [选项] [周期ms]\n", prog);
    printf("  (无选项)                 前台运行，Ctrl+C 退出（调试用）\n");
    printf("  --daemon, -d             后台守护运行（fork+setsid，子进程异常自动拉起）\n");
    printf("  --stop                   停止后台守护进程\n");
    printf("  --status                 查看运行状态（pid + 实时数据 + 数据来源）\n");
    printf("  --show-conf              打印当前生效配置（配置文件+命令行合并结果）\n");
    printf("\n设备配置（会覆盖 collector.conf）：\n");
    printf("  --conf <文件>            指定配置文件（默认 ./%s）\n", CONF_FILE);
    printf("  --host <IP>              Modbus TCP 设备IP\n");
    printf("  --port <端口>            设备端口（默认5020，真实设备常为502）\n");
    printf("  --slave <地址>           从机地址（默认1）\n");
    printf("  --transport tcp|rtu      传输方式（默认tcp）\n");
    printf("  --serial <设备>          RTU 串口节点，如 /dev/ttyUSB0\n");
    printf("  --baud <波特率>          RTU 波特率（默认9600）\n");
    printf("  --reg-temp <地址>        温度寄存器地址（默认0）\n");
    printf("  --reg-humi <地址>        湿度寄存器地址（默认1）\n");
    printf("  --scale-temp <值>        温度=原始值/该值（默认10）\n");
    printf("  --scale-humi <值>        湿度=原始值/该值（默认10）\n");
    printf("  --interval <ms>          采集周期（默认2000）\n");
    printf("  --threshold <℃>         温度告警阈值（默认32）\n");
    printf("  --real                   标记为真实设备（默认标记为模拟数据）\n");
    printf("  --sim                    标记为模拟数据\n");
    printf("  --source <名字>          数据来源名字，显示在网页/状态/日志里\n");
    printf("  --help, -h               显示帮助\n");
    printf("\n例（接真实设备，不改代码不重编译）：\n");
    printf("  %s --host 192.168.1.100 --port 502 --real --source \"车间1号温湿度计\" --daemon\n", prog);
}

int main(int argc, char *argv[])
{
    int i;
    int daemon_mode = 0;
    int want_conf_print = 0;

    cfg_defaults();
    prepare_paths();

    // 先读配置文件（命令行随后覆盖）
    if(cfg_load_file(g_conf_path) != 0)
    {
        printf("[配置] 没找到 %s，使用内置默认值（连接本机模拟器 127.0.0.1:5020）\n", g_conf_path);
        g_conf_path[0] = '\0';
    }

    openlog(SYSLOG_IDENT, LOG_PID | LOG_CONS, LOG_DAEMON);

    for(i=1;i<argc;i++)
    {
        const char *a = argv[i];
        const char *next = (i+1 < argc) ? argv[i+1] : NULL;
        #define NEED_VAL() do{ if(next == NULL){ fprintf(stderr,"%s 后面缺少参数值\n", a); return 2; } }while(0)

        if(!strcmp(a,"--daemon") || !strcmp(a,"-d")) daemon_mode = 1;
        else if(!strcmp(a,"--stop"))   { do_stop();   return 0; }
        else if(!strcmp(a,"--status")) { do_status(); return 0; }
        else if(!strcmp(a,"--show-conf")) { want_conf_print = 1; }
        else if(!strcmp(a,"--help") || !strcmp(a,"-h")) { usage(argv[0]); return 0; }
        else if(!strcmp(a,"--real"))   { g_cfg.source_simulated = 0; if(!strcmp(g_cfg.source_name,"modbus-simulator")) snprintf(g_cfg.source_name,sizeof(g_cfg.source_name),"modbus-device"); }
        else if(!strcmp(a,"--sim"))    { g_cfg.source_simulated = 1; }
        else if(!strcmp(a,"--conf"))   { NEED_VAL(); i++; cfg_load_file(next); snprintf(g_conf_path,sizeof(g_conf_path),"%s",next); }
        else if(!strcmp(a,"--host"))   { NEED_VAL(); i++; cfg_apply_kv("host", next); }
        else if(!strcmp(a,"--port"))   { NEED_VAL(); i++; cfg_apply_kv("port", next); }
        else if(!strcmp(a,"--slave"))  { NEED_VAL(); i++; cfg_apply_kv("slave", next); }
        else if(!strcmp(a,"--transport")) { NEED_VAL(); i++; cfg_apply_kv("transport", next); }
        else if(!strcmp(a,"--serial")) { NEED_VAL(); i++; cfg_apply_kv("serial", next); }
        else if(!strcmp(a,"--baud"))   { NEED_VAL(); i++; cfg_apply_kv("baud", next); }
        else if(!strcmp(a,"--reg-temp")) { NEED_VAL(); i++; cfg_apply_kv("reg_temp", next); }
        else if(!strcmp(a,"--reg-humi")) { NEED_VAL(); i++; cfg_apply_kv("reg_humi", next); }
        else if(!strcmp(a,"--scale-temp")) { NEED_VAL(); i++; cfg_apply_kv("scale_temp", next); }
        else if(!strcmp(a,"--scale-humi")) { NEED_VAL(); i++; cfg_apply_kv("scale_humi", next); }
        else if(!strcmp(a,"--interval"))   { NEED_VAL(); i++; cfg_apply_kv("interval_ms", next); }
        else if(!strcmp(a,"--threshold"))  { NEED_VAL(); i++; cfg_apply_kv("threshold", next); }
        else if(!strcmp(a,"--source"))     { NEED_VAL(); i++; cfg_apply_kv("source_name", next); }
        else if(atoi(a) > 0) g_cfg.interval_ms = atoi(a);      // 兼容老用法：直接给周期
        else { fprintf(stderr, "未知参数: %s\n", a); usage(argv[0]); return 2; }
        #undef NEED_VAL
    }

    if(want_conf_print) { cfg_print(); return 0; }

    // 参数合法性兜底
    if(g_cfg.interval_ms < 100)   g_cfg.interval_ms = 100;
    if(g_cfg.interval_ms > 60000) g_cfg.interval_ms = 60000;
    if(g_cfg.scale_temp == 0.0f)  g_cfg.scale_temp = 1.0f;
    if(g_cfg.scale_humi == 0.0f)  g_cfg.scale_humi = 1.0f;

    if(daemon_mode)
    {
        pid_t old = read_pid_file();
        if(old > 0 && process_alive(old))
        {
            printf("采集守护进程已经在运行了 (pid=%d)，如需重启请先 --stop\n", (int)old);
            return 3;
        }
        if(daemonize() != 0)
        {
            fprintf(stderr, "守护化失败: %s\n", strerror(errno));
            return 1;
        }
        return supervise_loop();
    }

    printf("[配置] 设备=%s%s  来源=%s(%s)  周期=%dms\n",
           strcmp(g_cfg.transport,"rtu")==0 ? g_cfg.serial : g_cfg.host,
           strcmp(g_cfg.transport,"rtu")==0 ? "" : "",
           g_cfg.source_name, g_cfg.source_simulated ? "模拟数据" : "真实设备", g_cfg.interval_ms);
    return worker_main();
}
