# 按钮与 GUI 导航说明

项目使用 GPIO4 主按钮，以及摇杆的 B/X/Y 三路输入。B 是 GPIO21 上低电平
有效的数字按键；X/Y 分别是 GPIO20/19 上的 ADC 模拟轴。数字输入执行 30 ms
软件去抖，模拟轴使用中心校准、死区、迟滞和重复节流。

| 输入 | GPIO | 默认 GUI 功能 |
|---|---:|---|
| 主按钮 | 4 | 松开时确认当前焦点 |
| B | 21 | 可重映射的辅助确认键 |
| X | 20 | ADC 横轴，产生左/右 |
| Y | 19 | ADC 纵轴，反转极性后产生上/下 |

焦点包含内容区控件和底部“首页”按钮。每个 App 使用显式四方向邻接表。
确认“首页”后进入 App 主导航；在主导航确认图标后打开对应 App。主按钮
短按与 B 当前默认等价，但两者保留独立事件，后续 App 可单独接管摇杆 B。
X/Y 只产生方向事件，绝不产生确认事件。

## 闲聊闹钟 App 特殊手势

闲聊闹钟 App 打开时，主按钮长按 800 ms 被 App 接管：

1. 检查音频是否空闲以及 Wi-Fi 是否可用；
2. 调用 `voice_chat_start()` 开始录音；
3. 按住期间由 `pump_voice()` 持续上传音频；
4. 松开后调用 `voice_chat_stop_and_process()`；
5. 完成豆包 ASR、Agent 和 TTS 链路。

录音最长 10 秒。达到上限后自动停止并开始处理。长按已触发录音时，
松开不会再执行当前焦点的普通确认动作。

在其它 App 或主导航中，主按钮长按没有特殊语义，松开时仍按普通确认
处理。这为以后网络电台和游戏 App 保留了自定义长按或方向输入的空间。

## 设置 App

设置 App 提供四个可聚焦操作：

| 项目 | 确认动作 |
|---|---|
| 安静模式 | 开启或关闭自动整点报时和自动随机发言 |
| WiFi 设置 | 扫描附近网络、选择历史网络或手动输入 SSID/密码 |
| API 设置 | 开启只用于 API Key 的 SoftAP 配置网页 |
| 时间同步 | 立即通过 Wi-Fi 执行 SNTP 校时并写入 DS3231 |

原来的主按钮双击安静模式手势已经移除，避免与 GUI 的即时确认语义冲突。

WiFi 虚拟键盘中，摇杆移动按键焦点，确认按钮输入当前按键。键盘包含
文本光标左移/右移、退格、大小写/数字符号切换、确定和取消键。

## API SoftAP 配置模式

API 设置页显示 `talkingflower` 热点名称、密码、网页 URL 和退出按钮。
确认退出按钮或在该页长按主按钮都会关闭配置热点，并尝试恢复当前 Wi-Fi。

## 配置参数

- `APP_BUTTON_PIN = 4`
- `APP_JOYSTICK_BUTTON_PIN = 21`
- `APP_JOYSTICK_X_PIN = 20`
- `APP_JOYSTICK_Y_PIN = 19`
- `APP_JOYSTICK_X_INVERTED = false`
- `APP_JOYSTICK_Y_INVERTED = true`
- `APP_BUTTON_DEBOUNCE_MS = 30`
- `APP_BUTTON_LONG_PRESS_MS = 800`
- `APP_VOICE_MAX_RECORD_MS = 10000`

## 实现位置

| 位置 | 职责 |
|---|---|
| `main/buttons.c` | GPIO 初始化、低电平读取、软件去抖和按下/松开事件 |
| `main/gui_shell.c` | 默认焦点移动、首页、主导航和确认路由 |
| `main/app_main.c:handle_input_event()` | 把 B/X/Y/主按钮转换为 GUI 输入 |
| `main/app_main.c:handle_button_long_press()` | 闲聊闹钟录音和配置模式兼容长按 |
| `main/app_settings.c` | 设置项及其业务动作 |
