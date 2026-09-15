/* ============================================================
   config.js —— 前端全局配置
   对接自研 webserver 时只需修改此处
   ============================================================ */

window.APP_CONFIG = {
  /* 是否启用模拟数据模式（已切 false: 直连C后端）
     true  —— 不依赖后端，浏览器本地生成模拟数据（后端未完成时用）
     false —— 通过 fetch 请求自研 webserver 的 RESTful 接口 */
  MOCK: false,

  /* webserver 接口前缀。webserver 与页面同源部署时保持 '/api' 即可 */
  API_BASE: '/api',

  /* 实时数据轮询间隔（毫秒）
     对应架构：webserver 读共享内存 → 返回给浏览器 */
  POLL_INTERVAL: 2000,

  /* 实时曲线最多保留的数据点数量（约 POLL_INTERVAL * 150 ≈ 5 分钟） */
  REALTIME_POINTS: 150,

  /* 历史查询默认时间跨度（小时） */
  HISTORY_DEFAULT_HOURS: 24,

  /* 模拟数据初始值，仅 MOCK = true 时生效 */
  MOCK_INIT: { temp: 24.5, humi: 55.0 },

  /* 告警阈值默认值 */
  THRESHOLD: { tempMax: 32, tempMin: 5, humiMax: 85 }
};
