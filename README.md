# 淇喵盒子 (qimiaoBox)

ESP32-C6 驱动的 ST7789 240×240 SPI LCD 智能桌面摆件，基于 LVGL 9.3 的触控/按键 UI。

## 硬件

| 组件 | 型号 |
|------|------|
| MCU | ESP32-C6 |
| 屏幕 | ST7789 240×240 SPI LCD |
| 输入 | 4 按键 (ESC / ENTER / DOWN / UP, GPIO 18-21) |

### 引脚

| 信号 | GPIO |
|------|------|
| SCLK | 0 |
| MOSI | 7 |
| LCD_DC | 4 |
| LCD_RST | 6 |
| LCD_CS | 5 |
| KEY_ESC | 18 |
| KEY_ENTER | 20 |
| KEY_DOWN | 21 |
| KEY_UP | 19 |

## 构建

需要 ESP-IDF v6.0.1 环境。

```bash
idf.py build flash monitor
```

VS Code 配置见 `.vscode/settings.json`（目标 `esp32c6`，端口 `COM21`，JTAG 烧录）。

## 功能

### 页面

| 页面 | 描述 | 进入方式 |
|------|------|----------|
| Splash | "淇喵盒子" + 构建时间，1s | 启动 |
| Key Test | 四色方框对应按键，按下变色 | splash 后自动 |
| Birthday | 生日倒计时，4行逐行淡入 | key_test 长按 ESC（需 NTP 同步） |
| Time Sync | WiFi 扫描 + NTP 多服务器轮询 | key_test 长按 ESC（未同步时） |
| Heart | 500 粒子从边缘飞向心形轮廓 | birthday 短按 DOWN |

### 控制台命令

USB 连接后，命令提示符 `miao>` 可用:

```bash
setcfg wifi.ssid.1 str <SSID>    # 配置 WiFi（支持 1-5 组）
setcfg wifi.pwd.1 str <密码>
setcfg ntp.timeout int 120000    # NTP 超时 (ms), 默认 60s
getcfg <key>                     # 读取配置
factoryreset                     # 擦除全部 NVS + 重启
reboot                           # 重启
```

### NTP 时间同步

13 台服务器轮询: 国家授时中心 → 阿里云 → 腾讯云 → 百度云 → 华为云 → 教育网 → pool.ntp.org。单台 10s 超时，总超时可配。

WiFi 凭据支持多组 (wifi.ssid.{1-5})，连接前先扫描周围 AP 取第一个匹配的。

## 依赖

| 组件 | 版本 |
|------|------|
| ESP-IDF | 6.0.1 |
| LVGL | 9.3.0 |
| espressif/button | 4.2.0 |

## 项目结构

```
main/
  app_main.c         硬件初始化 + 主入口
  miaobox_ui.c       LVGL UI (4 页面 + splash)
  miaobox_net.c      WiFi 扫描匹配 + NTP 同步
  miaobox_console.c  控制台 (USB Serial JTAG, 异步探测)
  miaobox_cmd.c      控制台命令 (setcfg/getcfg/delcfg/reboot/factoryreset)
  lv_font_alibaba_22.c  字体 (阿里巴巴普惠体 22px)
font/
  Alibaba-PuHuiTi-Regular.ttf  源字体
```

## 许可

MIT
