# Changelog

## [Unreleased]

### Added
- **主菜单页面**: 横向滚动菜单，6 项图标/文字缩放动画，UP/DOWN 左右移动，ENTER 进入子页，预留项 toast 提示
- **设置页面**: 暗色调可滚动面板，调试模式/保持WiFi/帧率显示 switch，NTP 超时 dropdown，重启/恢复出厂 action，UP/DOWN 行选择高亮
- **AI 猫猫画图模块** (`miaobox_ai.c/.h`): WiFi→阿里云 AI API 请求→解析 JSON→HTTP 下载图片→逐步写入 Flash 分区→CRC32 校验，全程 8KB 小缓冲不占 RAM
- **猫猫图片显示页**: PNGdec 逐行解码 PNG→RGB565→LVGL 渲染，根据堆内存动态决定输出尺寸
- **菜单图标系统**: 设置/调试/默认 3 枚 200×200 预转换 LVGL 图标
- **PNGdec 解码库** (`main/pngdec/`): 轻量 PNG 解码器，支持逐行回调输出
- 分区表新增 512KB `storage` 分区 (猫猫图片存储)
- SDK config 新增 LVGL PNG 支持 (`CONFIG_LV_USE_LODEPNG`)

### Changed
- **控制台两阶段初始化**: `console_init()` 仅探测 USB，`console_start_repl()` 在硬件/WiFi 初始化完成后创建 REPL task，避免 linenoise 提示符与日志洪峰竞争 TX buffer
- **主控制台切换为 USB Serial JTAG** (`sdkconfig`): 修复 stdin 绑定在 UART0 导致 console 无响应的问题
- **网络模块重构** (`miaobox_net.c/.h`): 提取 `wifi_connect_core()` / `wifi_disconnect_core()` 共享逻辑，EventGroup 替代状态标志位，AI/NTP 模块复用同一 WiFi 连接
- **字体更新**: 新增主菜单/设置页/猫猫页所需汉字
- **按键路由重构**: 主菜单页/设置页各自处理按键，ESC 长按统一返回主菜单

### Fixed
- 串口控制台 `miao>` 无响应 (Writing to serial is timing out)
- NTP 同步失败时 WiFi 未释放
