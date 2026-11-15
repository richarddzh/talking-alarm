# esp32_wifi_alarm_idf

[`esp32_wifi_alarm/`](../esp32_wifi_alarm) 项目的 **ESP-IDF v5.3 LTS** 重写版。
硬件使用 ST7789 TFT、麦克风、扬声器和按钮；配置方式从 BLE GATT 改为设备自建 Wi-Fi 热点 + HTTP 页面。ESP-IDF 版的目标是更精细的内存控制和更稳定的实时音频。

## 为什么重写

Arduino 版在录音/播放时丢帧明显，主要原因是 Arduino-ESP32 在 `loop()`
里塞了太多事情，I2S DMA 和 WiFi/TLS 抢同一个核同一个调度槽。本版改用：

- 单独的 audio task 钉在 **Core 1**，与 Wi-Fi/lwIP/main 完全分核
- 新版 `driver/i2s_std.h` 接口，DMA buffer 数量/大小可单独调
- `esp_tls` 原生连接，录音经 PSRAM ring 分块上传，复用 WebSocket 工作区
- 原生 320×240、16 色索引 GUI；38.4 KB framebuffer 放在 PSRAM
- 完整 GNU Unifont BMP 中文点阵字体 + UTF-8 渲染
- 模块化 App Shell、手机式图标主导航和固定底部状态栏
- Espressif 官方 MP3 解码器驱动的网络电台 App
- 原图精灵 Boids 小鱼缸、带实际时间表盘及豆包语音提醒的 25/5/15 番茄时钟
- `esp_wifi` + `esp_event` 直接控制 Wi-Fi STA/AP 切换；连接失败时自动进入设备自建热点配置模式

## Core 分工（已在 sdkconfig.defaults 显式声明）

新增 App 操作、原图处理和后台计时规则见
[docs/aquarium_pomodoro.md](docs/aquarium_pomodoro.md)；
启动器七个 App 使用三列两行滚动窗口，向下可到达番茄时钟。

| 任务                    | 默认/配置                              | 实际所在 Core |
|-------------------------|----------------------------------------|----------------|
| `app_main` / 主循环     | `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y` | **Core 0**    |
| `voice_worker` / `chime_worker` / `radio_player` / `radio_http` | `xTaskCreatePinnedToCoreWithCaps(..., 0, MALLOC_CAP_SPIRAM)` | **Core 0** |
| Wi-Fi 任务              | `CONFIG_ESP_WIFI_TASK_CORE_ID=0`       | **Core 0**    |
| lwIP TCP/IP 任务        | `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y` | **Core 0**  |
| `audio_io` （I2S 收发） | `xTaskCreatePinnedToCore(..., 1)` + `configMAX_PRIORITIES - 1` | **Core 1** |

也就是说，**所有阻塞型 I/O（Wi-Fi / TLS 握手 / HTTP 收发 / SPIFFS 读写 / TFT SPI 刷屏）都在 Core 0**，麦克风采集 + 解码降采样 +
扬声器输出 + DMA 等待都由 Core 1 上的 `audio_io` 任务负责，而且它使用
**最高 FreeRTOS 任务优先级**。即使 Core 0 上 TLS 握手卡 200 ms，也不会让
Core 1 的 I2S 出现 DMA 溢出或欠运行；当然，硬件中断仍然可以抢占任何任务。

## 缓冲区与无锁同步

只有两条数据流，且每条数据流都是「一个生产者 + 一个消费者」，没有
任何 buffer 会被两个任务同时写。因此整个音频路径是 **完全无锁的
SPSC ring**（`main/ring.[ch]`），同步只靠：

- 32-bit 对齐的 `volatile uint32_t head/tail` —— Xtensa LX6 单字读写原子
- `__sync_synchronize()` 全屏障 —— 保证生产者写完数据再让 head 推进，
  消费者看到新 head 后再去读数据

| 阶段       | 生产者              | 消费者              | 缓冲 |
|-----------|---------------------|---------------------|------------------------------|
| RECORD    | audio task (Core 1) | voice pipeline (Core 0) | 共享 PSRAM arena 的前 64 KB |
| PLAY      | TTS 或 MP3 worker (Core 0) | audio task (Core 1) | 256 KB PSRAM PCM ring |

由于 RECORD 和 PLAY **永远不会同时发生**（长按单按钮录音 → 松开 → 上传 →
等回包 → 播放，是一个完整的串行序列），所以同一个 ring 控制结构按阶段
切换同一块 256 KB PSRAM 的有效容量：REC 使用前 64 KB，PLAY 使用全部
256 KB。读写 API 校验 owner、role 和取消 epoch，防止不同业务交叉写入。
I2S DMA 和 REC/PLAY 复用的 2 KB 搬运区始终留在内部 DRAM。电台另外使用 32 KB PSRAM
压缩数据 ring，将网络读取和 MP3 解码等待分离。
业务在网络连接前领取 owner，全部生产者和句柄清理后释放；不能只看 phase
为 IDLE 就认为音频工作区可用。

主循环、worker 和 Core 1 音频阶段的完整状态、超时取消及错误恢复规则见
[docs/audio_state_machine.md](docs/audio_state_machine.md)。

## 自动休眠与自动息屏

设置页新增「自动休眠」（永远不、5、10、30 分钟）和「自动息屏」
（永远不、1、5、10 分钟），向下滚动设置列表并按确认循环切换。
默认均关闭，选项保存到 `/spiffs/power_settings.txt`，通过临时文件和备份
替换；保存失败不会修改当前选项。启动时可恢复中断写入留下的有效备份，
无效配置会记录错误并禁用自动省电。

两个超时独立地从最后一次物理输入计时，休眠优先，息屏不重置计时。
休眠取消录音、语音请求、当前播报和电台，退出游戏及配置热点；
只有整点报时可临时启用音频，完成后恢复静音，屏幕保持关闭。
息屏继续现有音频并退出游戏；整点播报期间暂时让电台停止，完成后恢复。
整点报时仍受安静模式、现有报时时段和网络可用性约束。
任何按钮或摇杆方向可唤醒，唤醒动作及其松开/连发被消费，不会误触功能。
被休眠取消的旧网络请求即使稍后收到响应，也不会在唤醒后突然播放。
启动时已经为低电平的按键不算用户持续操作；释放后再次按下即可正常使用。
启用自动省电时，串口每 30 秒记录实际空闲时间、超时设置和输入状态，
可区分设置问题、持续输入和显示硬件问题。

显示驱动发送 ST7789 DISPOFF/SLPIN，唤醒后 SLPOUT/DISPON 并重新绘制，
关闭期间不再提交 GUI 帧。`APP_TFT_BACKLIGHT_PIN` 默认 `-1`（没有可控背光），
若需要切断 LED 背光电流，必须按模块规格接线并配置该 GPIO 及有效电平
`APP_TFT_BACKLIGHT_ON`，不能仅靠显示控制器休眠。固件不进入 ESP32 深度睡眠，
以便保留摇杆 ADC 检测和整点报时。

主机端计时/文件保存回归测试（使用现有 ESP-Clang，无须设备）：

```powershell
python tools\test_idle_power.py --clang C:\Espressif\tools\esp-clang\16.0.1-fe4f10a809\esp-clang\bin\clang.exe
```

以上命令从仓库根目录执行。硬件验收应覆盖每个超时档位、游戏退出、电台息屏
续播/整点恢复、休眠停止录音与延迟 TTS、按钮长按/松开和摇杆回中唤醒，
以及断电重启后设置恢复。背光实际省电效果需要接线后测量。


## 内存预算与扩展约定

当前内存布局、缓冲生命周期、DMA/PSRAM 边界和新 App 接入规则统一维护在
[docs/memory_analysis.md](docs/memory_analysis.md)。
大工作区、ASR/Agent/TTS 网络对象及 Core 0 worker 栈显式放在 PSRAM；
内部 SRAM 留给 I2S/SPI DMA、实时搬运、`audio_io` 栈和系统必需对象。
普通 `malloc()` 不保证分配到 PSRAM，分配失败不得静默回退挤占内部堆。

本次构建应用静态 RAM 从 79127 B 降至 9503 B，释放约 68 KB 内部 RAM；
worker 栈迁移额外降低运行时内部堆占用。精确预算需核对当前 ELF 和实际
生成配置，不能把这组数值作为未来版本的固定额度。

`APP_AUDIO_ARENA_BYTES` 控制 REC 有效容量，`APP_SPEAKER_BUFFER_BYTES`
控制共享 PSRAM 分配及 PLAY 容量。两者必须为 2 的幂，REC 不得超过 PLAY；
新增预填充目标必须为下一帧留出空间。I2S DMA 数量不能随 PSRAM 容量盲目放大。
TLS 外部内存不改变已有证书校验策略；需要保护外部 RAM 中敏感数据时，
应按 ESP-IDF 安全指南配置 Flash/PSRAM 加密。

切到 SoftAP 配网模式时暂停音频业务，HTTP 配网页工作区与音频业务不并行。

### 如何用日志验证当前预算合不合理

代码里在所有关键事件点都打了一行 `mem_log()`（实现见 `main/mem_log.[ch]`），
日志格式：

```
I (12345) mem: <tag> heap=NNN min=NNN int=NNN dma=NNN dma_max=NNN
```

| 字段 | 含义 |
|---|---|
| `heap`    | 当前空闲堆（含 PSRAM）|
| `min`     | **启动到现在的历史最低空闲堆** —— 决定能不能再放大 buffer 的关键指标 |
| `int`     | `MALLOC_CAP_INTERNAL` 当前空闲（排除 PSRAM）|
| `dma`     | `MALLOC_CAP_DMA` 当前空闲（Wi-Fi 与 I2S DMA 共享池）|
| `dma_max` | `MALLOC_CAP_DMA` 最大连续可分配块 —— 碎片化指标 |

打点时机：

| tag                | 触发场景                          |
|--------------------|-----------------------------------|
| `boot done`        | 启动流程结束                      |
| `audio_io_init`    | audio task 创建完，共享 PSRAM arena 已分配 |
| `radio PCM ready`  | MP3 首帧解码，准备开始 PCM 预缓冲 |
| `mic install`      | I2S0 mic DMA 分配后                |
| `spk install`      | I2S1 spk DMA 分配后                |
| `REC stop`         | 一次录音收尾、mic DMA 释放         |
| `PLAY stop`        | 一次播放收尾、spk DMA 释放         |
| `voice tls pre`    | TLS 握手开始前                    |
| `voice tls post`   | TLS 握手 + 发送 POST 头之后        |
| `voice done`       | 一轮 voice chat 完整结束          |
| `ap pre/on/off`     | SoftAP 配网切换前后              |
| `wifi back`        | 配网 AP 关闭后 Wi-Fi 重连完成     |
| `periodic`         | 每 5 分钟一次的健康度快照         |

调参三条经验法则：

1. 多轮操作结束后 `heap/int` 不能恢复，且 `min` 持续下降 → 排查泄漏；
   单独历史最低值下降不能证明泄漏。
2. `voice tls post` 的 `min` 比 `voice tls pre` 掉 30 KB 以上 → TLS 握手
   是当前内存峰值，应同时检查内部能力堆与 PSRAM 分配落点。
3. `mic install` / `spk install` 后 `dma_max` 显著低于 `dma` 总量
   → DMA 池开始碎片化，不要再加 dma_desc_num。

## 网络与音频协议分工

| 维度                  | SNTP 网络校时                      | Doubao ASR + Agent + TTS                   |
|----------------------|------------------------------------|-------------------------------------------|
| 协议                  | UDP/NTP                            | WebSocket + HTTPS                           |
| 数据形态              | UTC 时间戳                         | PCM、文本和 base64 PCM                      |
| 实时性要求            | 低                                 | 高                                          |
| 运行频率              | 启动及每 24 小时                   | 用户语音、报时或手动随机发言时              |

具体到 `esp_http_client` 的限制：

1. **不支持「边产生边发」的 chunked POST**。它要求要么 `set_post_field`
   把 body 一次性传完，要么用 `is_chunked_data_set` + `set_header
   Transfer-Encoding: chunked`，但 `perform()` 会在内部按写入再发送的
   节奏跑，不适合「ring 里来一段就 chunk 一段」的实时录音场景。
2. **响应 body 要么进 event handler 里 memcpy 出来，要么一次性 read 到
   buffer**。我们需要把响应的 PCM 直接 push 到 SPSC ring，由 audio
   task 同步消费，并对「ring 满 → 阻塞 producer」做精确的超时控制。
   走 event handler 等于在 lwIP 任务上下文里阻塞，不安全。
3. **额外的状态机和 heap 开销**。`esp_http_client_handle_t` 自带头部
   缓冲、URL 解析、重定向、Auth 等一整套，平均 8–12 KB 临时堆，对我们
   严格的内存预算不友好。

具体分工：

- **`wifi_time.c`** —— 通过 `esp_netif_sntp` 从 `ntp.aliyun.com` 获取 UTC，
  使用 `CST-8` 转成中国标准时间，再写入 DS3231。
- **`voice_chat.c`** —— 单按钮长按进入录音后，把 mic ring 中的 16 kHz/16-bit/mono
   PCM 分片写入 `doubao_asr_client.c` 的 WebSocket 二进制协议；松开后拿
   ASR 文本调用 `doubao_agent_client.c`，最后交给本机豆包 TTS 播放器。
- **`chime_player.c`** —— 单击按钮、整点报时、随机自发发言都直接调用
   `doubao_agent_client.c` 获取要朗读的文本，再交给本机豆包 TTS 播放器。
- **`doubao_asr_client.c`** —— 直连豆包 ASR `bigmodel_nostream` WebSocket，
   发送未压缩 PCM ASR 二进制帧，解析最终识别文本。
- **`doubao_agent_client.c`** —— 直连联网问答 Agent Chat Completion API，
   使用标准模式（不传 `model`）和上海浦东新区位置参数。
- **`doubao_tts_player.c`** —— 直连豆包 TTS API，消费返回 JSON 字节流，
   只识别 `data` 字段，把 base64 PCM 每 4 个字符一组解码并推进 spk ring，
   不缓存整行 JSON，也不经过 App Service 代理音频流。
- **`radio_player.c`** —— 在 Core 0 用 `esp_http_client` 读取 MP3 直播流，
  通过 Espressif `esp_audio_codec` 解码为 PCM，预缓冲后交给 Core 1 I2S；
  采样率和声道由首个解码帧动态配置。

### Doubao API keys

ESP32 直接调用豆包 ASR、Agent 和 TTS。API key 不再通过编译期 `main/app_secrets.h` 注入，而是通过配置网页写入 SPIFFS：

```text
/spiffs/app_secrets.txt
<doubao asr api key>
<doubao agent api key>
<doubao tts api key>
```

运行时由 `main/app_secret_store.[ch]` 加载，ASR / Agent / TTS 客户端在发请求时读取。其它非 secret 参数仍在 `main/app_config.h` 里定义，包括 ASR resource id、Agent bot id、上海位置参数、TTS resource id / speaker / sample rate。

## ESP32-S3 N16R8 引脚

| 用途                       | 引脚           |
|----------------------------|----------------|
| 独立主确认按钮（低电平有效） | GPIO 4 |
| 摇杆 B 辅助确认（低电平有效） | GPIO 21 |
| 摇杆 X / Y 模拟轴 | GPIO 20 / 19（ADC） |
| DS3231 I2C SDA / SCL       | GPIO 8/9       |
| ST7789 SCL / SDA / DC / CS | GPIO 11/12/13/14 |
| 麦克风 BCLK / LRCK / SD    | GPIO 2/1/42    |
| 扬声器 BCLK / LRC / DIN    | GPIO 41/40/39  |
| 板载 RGB LED               | GPIO 48（启动时关闭） |

GPIO19/20 用作摇杆 ADC，原生 USB Serial/JTAG 次级控制台已关闭；日志使用
默认 UART 控制台。GPIO39–42 同时是默认 JTAG 信号脚，当前固件将其复用为
I2S，因此也不要连接外部 JTAG 到这些引脚。

ST7789 按 320×240 横屏初始化，SPI 时钟为 40 MHz。模块的 GND、VCC 和
背光脚需要按模块规格另外接线；当前接线未提供硬件 RESET，驱动使用软件
复位。设备只使用 TFT 显示，GUI 直接按 320×240 原生分辨率绘制。
4-bit 索引色 framebuffer 放在 ESP32-S3 N16R8 的 8 MB PSRAM 中。

语音链路现在是：
长按 GPIO4 单按钮录音并上传 raw PCM 到豆包 ASR，ESP32 收到识别文本后调用豆包 Agent，再
调用豆包 TTS 并播放返回的 PCM。Chime 链路现在是：ESP32 直接调用豆包
Agent 生成报时或随机发言文本，再直连豆包 TTS 播放。

自动 chime 调度：

- 整点报时安排在 `07:00` 至 `23:00`。每小时只在 `:00:00` 到
  `:04:59` 之间等待音频空闲并尝试启动；到 `:05:00` 仍未启动则放弃，
  不会延迟补报。
- **自动随机发言当前暂时禁用**：`RANDOM_CHIME_COUNT = 0`，因此每小时
  规划 0 个目标时刻。5 槽位容量、`:05`–`:55` 时间窗和至少 8 分钟间隔
  的规划代码仍然保留；以后把数量改回 `1`–`5` 即可重新启用。
- 自动随机槽位到点时若音频忙，会在同一小时内继续等待；跨小时后不再
  补发。设备在小时中途启动时，已经过去的槽位会直接跳过。
- 整点报时向豆包 Agent 发送明确的 12 小时制 `AM/PM` 时间；随机发言和
  手动单击则从 23 条话题提示中随机选择，再由 Agent 动态生成播报文本。
- 安静模式改由“设置” App 切换；它暂停自动整点报时及自动随机发言路径，
  手动闲聊与长按语音仍可使用。当前自动随机数量为 0，因此不会产生随机槽位。

完整调度边界和随机话题设计见
[docs/rtc_chime_design.md](docs/rtc_chime_design.md)。

GUI 操作：

- X 模拟轴：横向移动焦点。
- Y 模拟轴：纵向移动焦点。
- 独立主按钮短按：确认当前焦点。
- 摇杆 B：默认作为可重映射的辅助确认键。
- X/Y 方向事件与确认事件在输入适配层分离，不会触发按钮按下动作。
- 只有闲聊闹钟 App 的动作卡会把确认映射为 Chime；其它 App 执行各自操作。
- 网络电台 App 中上下选择电台，确认键播放；再次确认当前电台即停止。
- 闲聊闹钟 App 内按住主按钮 800 ms：开始麦克风录音，松开后执行 ASR → Agent → TTS 并播放回复；最长录音 10 秒。
- 配置模式按住 800 ms：关闭临时热点并尝试连接已保存的 Wi-Fi；连接失败会自动回到配置模式。
- 正常联网时不能通过按钮主动打开配置热点；无凭据或连接失败时固件会自动进入配置模式。

完整的手势边界、失败行为和实现位置见
[docs/buttons.md](docs/buttons.md)。

## 分区表（16 MB flash）

```
nvs       0x9000   0x6000
otadata   0xf000   0x2000
phy_init  0x11000  0x1000
factory   0x20000  0x400000   (4 MB app slot)
ota_1     0x420000 0x400000   (4 MB OTA slot)
storage   0x820000 0x7E0000   (7.875 MB SPIFFS)
```

## 编译 / 烧录

```powershell
. 'C:\Espressif\tools\Microsoft.v5.3.5.PowerShell_profile.ps1'
cd esp32_wifi_alarm_idf_s3n16r8
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor
```

第一次启动时 SPIFFS 会自动格式化。设备“设置” App 现在把网络配置拆成
“WiFi 设置”和“API 设置”：

- “WiFi 设置”会扫描附近网络，同时列出最多 8 个历史网络；可直接选择
  已保存网络、选择扫描结果后输入密码，或手动填写隐藏 SSID。
- 屏幕虚拟键盘由摇杆移动焦点、任一确认按钮选键，支持文本光标左右移动、
  退格、大小写/数字符号切换、确定和取消。
- 旧 `/spiffs/wifi_config.txt` 会自动作为第一个历史网络使用，现有 Wi-Fi
  配置不会丢失；新的历史列表保存在 `/spiffs/wifi_profiles.bin`。
- “API 设置”才启动 SoftAP 网页：SSID `talkingflower`，密码
  `pangmiaomiao`，URL `http://192.168.4.1/`。

API 设置页会显示热点名、密码、URL 和“退出设置模式”按钮。网页只维护
Doubao ASR、Agent、TTS 三个 API Key，原有 `/spiffs/app_secrets.txt`
内容保持不变。退出 API 设置后，设备会使用当前选中的 Wi-Fi 配置恢复联网。

详细设计记录见 [docs/2026-06-21-softap-provisioning.md](docs/2026-06-21-softap-provisioning.md)。

### 后续迭代：不丢 Wi-Fi 配置的烧录方式

`main/CMakeLists.txt` 里**没有**调用 `spiffs_create_partition_image()`
（也没有 `FLASH_IN_PROJECT` 标志），意味着普通的烧录命令**不会**
往 SPIFFS 分区写任何东西，已保存的 `/spiffs/wifi_config.txt` 和 `/spiffs/app_secrets.txt` 会原样
保留。常用三种命令的影响如下：

| 命令                          | bootloader | partition table | app | spiffs (Wi-Fi/API key 配置) |
|-------------------------------|:----------:|:---------------:|:---:|:------------------:|
| `idf.py app-flash`            |     ❌     |       ❌        | ✅  |        ❌          |
| `idf.py flash` （默认）       |     ✅     |       ✅        | ✅  |        ❌          |
| `idf.py erase-flash` + `flash`|     ✅     |       ✅        | ✅  |  **✅ 整片擦除**   |

实际开发建议：

- **改了应用层 C 代码** → `idf.py -p COM6 app-flash monitor`
  - 只写 4 MB 的 app 分区，最快，且 100% 不动 SPIFFS。
- **改了 `sdkconfig.defaults` / 引脚 / 任务栈大小** → `idf.py -p COM6 flash monitor`
  - bootloader / partition_table 即使被一并写入，由于偏移和长度不变，
    SPIFFS 分区在 flash 上的物理区域不会被覆盖，配置依然存在。
- **千万不要改 `partitions.csv` 里 `storage` 那一行的 offset 或 size**。
  一旦偏移变了，原先 SPIFFS 区域会被新 partition 表里别的内容覆盖，
  Wi-Fi 和 API key 配置就丢了。如果不得不改，请通过自动启动的配置热点重新配置。
- **想强制清掉 Wi-Fi/API key 配置**：擦除 SPIFFS 但不动 app，可以单独用
  `parttool.py` 抹该分区：
  ```powershell
  parttool.py -p COM6 erase_partition --partition-name=storage
  ```
  完全不需要 `erase-flash`。

## 文件结构

```
esp32_wifi_alarm_idf/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── app_main.c          状态机 + UI 编排
    ├── app_config.h        引脚 / URL / SoftAP / 缓冲区大小
    ├── ring.[ch]           SPSC 字节 ring（无锁）
    ├── audio_io.[ch]       Core 1 I2S，共享 PSRAM 与业务 owner
    ├── voice_chat.[ch]     esp-tls chunked POST + body 直推 ring 播放
    ├── wifi_creds.[ch]     当前 Wi-Fi 与历史网络 SPIFFS 读写
    ├── app_secret_store.[ch] SPIFFS /app_secrets.txt 读写
    ├── wifi_time.[ch]      esp_wifi STA + esp_http_client 取时
    ├── wifi_provision.[ch] API Key SoftAP + HTTP 配网页
    ├── buttons.[ch]        双确认按钮去抖 + X/Y ADC 摇杆方向
    ├── gui_app.h           App 描述符、输入和动作接口
    ├── gui_shell.[ch]      主导航、焦点路由和底部状态栏
    ├── app_chat_alarm.[ch] 闲聊闹钟 App
    ├── app_radio.[ch]      网络电台列表与状态界面
    ├── radio_player.[ch]   Core 0 HTTP/MP3 解码 worker
    ├── radio_stations.[ch] 电台目录
    ├── app_settings.[ch]   Wi-Fi/API 设置与屏幕虚拟键盘
    ├── st7789.[ch]         ST7789 TFT 驱动
    ├── font5x7.[ch]        ASCII 5x7 字体（475 B）
    ├── font_zh16.[ch]      GNU Unifont 16×16 字体访问层
    ├── assets/             嵌入 Flash 的完整 BMP 字体文件
    └── ui_screen.[ch]      原生 320×240 索引色画布
```

## 已知限制

- TLS **完全不校验**服务器证书（参见上面的「TLS 安全选项」），与
  Arduino 版 `setInsecure()` 行为一致。如果要走严格证书校验，把
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`、`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=n`
  打开，并在两处 cfg 上挂 `crt_bundle_attach = esp_crt_bundle_attach`
  即可。
- 5x7 字体只覆盖 ASCII 0x20–0x7E；日期、时间和状态栏均使用 ASCII。
- 自动 chime 依赖 DS3231 提供有效本地时间；RTC 未设置或读取失败时，
  不会生成整点报时和随机发言计划。
- 网络校时当前每 24 小时执行一次，并写回 DS3231；音频活动期间会推迟。
- 网络校时使用 SNTP，不再依赖曾返回滞后时间的 timeapi HTTP 服务。
  详细说明见 [docs/sntp_time_sync.md](docs/sntp_time_sync.md)。

## TLS 安全选项 - 默认完全不校验

本版三个层面叠加，确保 esp_tls **不会**对 Doubao API 做任何
证书链解析、签名校验或吊销查询：

1. **Kconfig**：
   - `CONFIG_ESP_TLS_INSECURE=y`
   - `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`
   - `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n`（不打包 ~30 KB 根证书）
   - `CONFIG_MBEDTLS_HAVE_TIME=n` + `CONFIG_MBEDTLS_HAVE_TIME_DATE=n`
2. **运行时配置**（已在 `voice_chat.c` / `wifi_time.c` 落实）：
   - `cfg.skip_common_name = true`
   - `cfg.crt_bundle_attach = NULL`
   - `cfg.cacert_buf = NULL`
3. **效果**：esp_tls 内部把 mbedTLS 的 `authmode` 设为
   `MBEDTLS_SSL_VERIFY_NONE`，握手过程只做最小必要的协议解析（提取
   服务器公钥用于密钥交换），**不再加载/解析任何 CA、不验证证书签名、
   不检查域名/SAN、不检查时间有效性**。

收益：
- mbedTLS 握手期堆占用减少 ~10–15 KB
- 二进制减少 ~30 KB（不带根证书 bundle）
- 启动期不需要时间同步即可发起 HTTPS

风险：
- 任何中间人都可以伪装成 Doubao API，截获 PCM 或返回伪造内容
- 当前用例是家庭环境的玩具时钟 → 业务上可以接受；如果要部署到
  公开网络，请按上面「已知限制」的方法切回严格校验
