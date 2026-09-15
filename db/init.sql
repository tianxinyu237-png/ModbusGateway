-- ===================== 用户表 =====================
-- 如果 users 表已存在，则先删除
DROP TABLE IF EXISTS users;
-- 创建用户表：存储系统登录账号信息
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,    -- 主键ID，自增
    username TEXT UNIQUE NOT NULL,           -- 用户名，唯一不可重复，不能为空
    password_hash TEXT NOT NULL,             -- 密码哈希值（salt$sha256(salt+password)，不存明文）
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP -- 账号创建时间，默认当前时间
);

-- ===================== 传感器历史数据表 =====================
-- 如果 sensor_data 表已存在，则先删除
DROP TABLE IF EXISTS sensor_data;
-- 创建传感器数据表：存放温湿度采集记录
CREATE TABLE sensor_data (
    id INTEGER PRIMARY KEY AUTOINCREMENT,    -- 主键ID，自增
    temperature REAL NOT NULL,               -- 温度，浮点型，不能为空
    humidity REAL NOT NULL,                  -- 湿度，浮点型，不能为空
    device_status INTEGER DEFAULT 1,         -- 设备状态：0=离线，1=在线，2=故障，默认在线
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP -- 数据采集时间戳（UTC），默认当前时间
);

-- ===================== 运行日志表 =====================
DROP TABLE IF EXISTS logs;
-- 分级日志：DEBUG/INFO/WARN/ERROR，前端日志面板直接读这张表
CREATE TABLE logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    time DATETIME DEFAULT (datetime('now','localtime')),  -- 记录时间（本地时间）
    level TEXT NOT NULL,                     -- 级别：DEBUG/INFO/WARN/ERROR
    module TEXT,                             -- 模块：webserver/collect/modbus/mq/shm...
    message TEXT                             -- 日志内容
);
CREATE INDEX idx_logs_id_desc ON logs(id DESC);

-- ===================== SQLite 性能配置 =====================
-- 开启WAL预写日志模式，提升并发读写性能，读和写可以并行执行
PRAGMA journal_mode=WAL;
-- 设置数据库忙等待超时时间：5000毫秒（5秒）
-- 当数据库被其他连接锁定时，最多等待5秒再返回忙错误
PRAGMA busy_timeout=5000;
