# 按钮与 GUI 导航说明

项目使用 GPIO4 主按钮，以及接在 GPIO19/20/21 的 B/X/Y 三个摇杆相关
输入。所有输入均使用内部上拉，低电平表示按下，`main/buttons.c` 统一
执行 30 ms 软件去抖。

| 输入 | GPIO | 默认 GUI 功能 |
|---|---:|---|
| 主按钮 | 4 | 松开时确认当前焦点 |
| B | 19 | 移动到下一焦点 |
| X | 20 | 移动到上一焦点 |
| Y | 21 | 摇杆按下/确认当前焦点 |

焦点包含内容区控件和底部“首页”按钮。确认“首页”后进入 App 主导航；
在主导航确认图标后打开对应 App。主按钮与 Y 默认完全等价。

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

设置 App 提供三个可聚焦操作：

| 项目 | 确认动作 |
|---|---|
| 安静模式 | 开启或关闭自动整点报时和自动随机发言 |
| 网络配置 | 开启或关闭 SoftAP 配网页 |
| 时间同步 | 立即通过 Wi-Fi 执行 SNTP 校时并写入 DS3231 |

原来的主按钮双击安静模式手势已经移除，避免与 GUI 的即时确认语义冲突。

## SoftAP 配置模式

配置模式中 GUI 仍可导航，设置 App 可以关闭配置热点。为兼容原有操作，
主按钮长按 800 ms 也可以直接关闭配置热点并尝试使用已保存的 Wi-Fi
凭据联网；连接失败时会自动重新进入配置模式。

## 配置参数

- `APP_BUTTON_PIN = 4`
- `APP_BUTTON_B_PIN = 19`
- `APP_BUTTON_X_PIN = 20`
- `APP_BUTTON_Y_PIN = 21`
- `APP_BUTTON_DEBOUNCE_MS = 30`
- `APP_BUTTON_LONG_PRESS_MS = 800`
- `APP_VOICE_MAX_RECORD_MS = 10000`

## 实现位置

| 位置 | 职责 |
|---|---|
| `main/buttons.c` | GPIO 初始化、低电平读取、软件去抖和按下/松开事件 |
| `main/gui_shell.c` | 默认焦点移动、首页、主导航和确认路由 |
| `main/app_main.c:handle_button_event()` | 把 B/X/Y/主按钮转换为 GUI 输入 |
| `main/app_main.c:handle_button_long_press()` | 闲聊闹钟录音和配置模式兼容长按 |
| `main/app_settings.c` | 设置项及其业务动作 |
