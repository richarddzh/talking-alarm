# Talking Alarm

面向 ESP32-S3 N16R8 的模块化闲聊闹钟固件。设备使用 ST7789 320×240
屏幕、双核音频流水线、模拟摇杆、DS3231 RTC 和豆包 ASR / Agent / TTS，
并内置网络电台、设置、俄罗斯方块和贪吃蛇 App。

## 功能

- 闲聊闹钟：长按主按钮录音，执行 ASR → Agent → TTS；摇杆按钮可请求
  一次 Chime。用户文本、Agent 回复、手动 Chime、随机 Chime 和整点播报
  都进入统一聊天记录，以带猫咪头像和尖角的圆角气泡显示。
- 猫咪头像直接使用参考图猫头区域缩放并量化后的固定 32×32 位图，运行时
  逐像素贴图，不再用圆、线和三角形重新绘制。
- 启动器图标由参考原图直接生成 48×48 RGB565 位图，使用 8 位透明度与卡片
  背景混合后 1:1 输出，不经过 24×24 放大，也不受 16 色 UI 调色板限制。
- 二维焦点导航：摇杆 `X/Y` 是 ADC 模拟轴，分别生成左/右和上/下事件；
  `B` 是独立数字按键。每个 App 声明显式焦点邻接表，轴移动永不生成确认。
- Wi-Fi 状态栏：连接时显示向上的多圆弧扇形；断线时显示带叹号的空心扇形。
- TFT 使用上一帧差分和局部区域提交，避免内容变化时反复整屏刷新造成闪烁。
- 网络电台：内置动感101、Love Radio、经典947、上海交通广播和上海新闻
  广播；使用 Espressif 官方 MP3 解码组件，并按广播原始采样率输出 I2S。
- 俄罗斯方块：七种彩色形状、左侧游戏区、右侧积分/消行/下一块提示和开始按钮。
- 贪吃蛇：方向化头部、身体、转角和尾部块，绿色红斑蛇身、眼睛、舌头和红苹果。
- SoftAP 配网、SNTP 校时、DS3231 本地走时、整点播报和安静模式。

## 工程结构

```text
esp32_idf_s3n16r8/
├── main/
│   ├── app_main.c              设备编排、当前 App 事件循环调度
│   ├── buttons.[ch]            B 数字按键与 X/Y ADC 摇杆适配
│   ├── gui_app.h               App、动作和焦点邻接表接口
│   ├── gui_shell.[ch]          主界面、当前 App 路由、状态栏
│   ├── app_chat_alarm.[ch]     聊天气泡、历史和语音交互
│   ├── app_settings.[ch]       设置 App
│   ├── app_radio.[ch]          网络电台列表和播放状态
│   ├── radio_player.[ch]       Core 0 HTTP/MP3 解码 worker
│   ├── radio_stations.[ch]     集中管理电台目录
│   ├── app_tetris.[ch]         俄罗斯方块 App
│   ├── app_snake.[ch]          贪吃蛇 App
│   ├── ui_screen.[ch]          4-bit 画布和基础图元
│   ├── ui_cat_avatar.[ch]      原创蓝灰猫咪头像组件
│   ├── audio_io.[ch]           Core 1 高优先级 I2S
│   └── ...                     Wi-Fi、RTC、ASR、Agent、TTS
└── docs/
    ├── joystick_focus_navigation.md
    ├── games_design.md
    ├── display_refresh.md
    ├── network_radio_design.md
    └── modular_app_gui_design.md
```

每个 App 都提供独立的 `tick`、`render`、`activate` 和可选
`handle_input`。`gui_shell_tick()` 只调用当前打开 App 的事件循环；主界面
由 Shell 自己处理输入和绘制。游戏和 GUI 在 Core 0，音频 DMA/I2S 任务固定
在 Core 1 的最高优先级，避免网络、ADC 和游戏逻辑打断声音输入输出。

## 摇杆与按钮

默认引脚：

| 信号 | GPIO | 类型 | 行为 |
|---|---:|---|---|
| B | 21 | 数字低电平 | 可重映射的辅助确认键 |
| X | 20 | ADC | 左/右移动 |
| Y | 19 | ADC | 上/下移动 |
| 主按钮 | 4 | 数字低电平 | 短按确认；闲聊 App 长按录音 |

ADC 启动时自动采样中心值，并使用按下死区、释放迟滞、首次重复延迟和连续
重复间隔。需要反转安装方向时，修改 `APP_JOYSTICK_X_INVERTED` 或
`APP_JOYSTICK_Y_INVERTED`。当前硬件的 Y 轴已启用反转，使物理向上对应
GUI 的向上事件。GPIO19/20 已专用于 ADC，因此固件使用 UART
控制台，不启用占用同一组引脚的原生 USB Serial/JTAG 控制台。

确认事件只由独立主按钮或辅助 B 按键产生。只有闲聊闹钟的动作卡会把确认映射为
`GUI_ACTION_CHIME`；设置和游戏使用各自的确认动作，X/Y 轴不会触发 Chime。

## 构建

```powershell
. 'C:\Espressif\Initialize-Idf.ps1' -IdfId 'esp-idf-ab7213b7273352b64422b1f400ff27a0'
Set-Location 'C:\gitroot\talking-alarm\esp32_idf_s3n16r8'
idf.py build
```

烧录示例：

```powershell
idf.py -p COM6 flash monitor
```

更完整的硬件、配网、音频内存和安全说明见
[`esp32_idf_s3n16r8/README.md`](esp32_idf_s3n16r8/README.md)。

## 扩展新 App

1. 新建独立的 `app_xxx.[ch]`。
2. 定义控件的 `gui_focus_node_t` 邻接表。
3. 实现 `tick/render/activate/handle_input`，不要在 App 中读取原始 GPIO。
4. 在 `app_main.c` 注册描述符，并在 `main/CMakeLists.txt` 添加源文件。
5. 耗时网络工作放在 Core 0 worker；不要改变 Core 1 音频任务优先级。
