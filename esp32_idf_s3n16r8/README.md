# esp32_wifi_alarm_idf

[`esp32_wifi_alarm/`](../esp32_wifi_alarm) 项目的 **ESP-IDF v5.3 LTS** 重写版。
硬件使用 ST7789 TFT、麦克风、扬声器和按钮；配置方式从 BLE GATT 改为设备自建 Wi-Fi 热点 + HTTP 页面。ESP-IDF 版的目标是更精细的内存控制和更稳定的实时音频。

## 为什么重写

Arduino 版在录音/播放时丢帧明显，主要原因是 Arduino-ESP32 在 `loop()`
里塞了太多事情，I2S DMA 和 WiFi/TLS 抢同一个核同一个调度槽。本版改用：

- 单独的 audio task 钉在 **Core 1**，与 Wi-Fi/lwIP/main 完全分核
- 新版 `driver/i2s_std.h` 接口，DMA buffer 数量/大小可单独调
- `esp_tls` 原生连接 + 手写 chunked 协议，录音 ring → 网络是零拷贝
- 原生 320×240、16 色索引 GUI；38.4 KB framebuffer 放在 PSRAM
- 完整 GNU Unifont BMP 中文点阵字体 + UTF-8 渲染
- 模块化 App Shell、手机式图标主导航和固定底部状态栏
- `esp_wifi` + `esp_event` 直接控制 Wi-Fi STA/AP 切换；连接失败时自动进入设备自建热点配置模式

## Core 分工（已在 sdkconfig.defaults 显式声明）

| 任务                    | 默认/配置                              | 实际所在 Core |
|-------------------------|----------------------------------------|----------------|
| `app_main` / 主循环     | `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y` | **Core 0**    |
| `voice_worker` / `chime_worker` | `xTaskCreatePinnedToCore(..., 0)` | **Core 0** |
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

| 阶段       | 生产者              | 消费者              | 共享 64 KB arena 当作 |
|-----------|---------------------|---------------------|------------------------------|
| RECORD    | audio task (Core 1) | voice pipeline (Core 0) | mic ring                 |
| PLAY      | TTS worker (Core 0) | audio task (Core 1) | spk ring                     |

由于 RECORD 和 PLAY **永远不会同时发生**（长按单按钮录音 → 松开 → 上传 →
等回包 → 播放，是一个完整的串行序列），所以同一块 64 KB 内存先被录音
端用，后被播放端用。录音开始前将 ring 标记为 `MIC` 并重置；TTS 播放
预填充前将其标记为 `SPK` 并重置。读写 API 会校验 role，防止状态错误时
交叉破坏共享 arena。

主循环、worker 和 Core 1 音频阶段的完整状态、超时取消及错误恢复规则见
[docs/audio_state_machine.md](docs/audio_state_machine.md)。


## 内存预算评估

ESP32-S3 N16R8 带 512 KB SRAM、16 MB Flash 和 8 MB Octal PSRAM。
I2S DMA、Wi-Fi DMA 和关键任务栈仍需使用内部 SRAM；普通动态分配可按
ESP-IDF capability 规则使用 PSRAM。
当 **Wi-Fi + TLS + 录音** 三者同时活跃时（最紧张），DRAM 大致分布：

| 占用方                                 | 约略大小       |
|---------------------------------------|----------------|
| Wi-Fi static + 动态 RX/TX 缓冲         | 50 KB          |
| lwIP TCP 窗口（已调小）                | 15 KB          |
| mbedTLS 一次握手 + 应用流              | 20–30 KB *     |
| FreeRTOS 内核 + ROM 保留               | ~30 KB         |
| 本应用音频 arena                       | **64 KB**      |
| 本应用 I2S DMA（mic 12 KB + spk 8 KB） | **20 KB**      |
| 本应用各任务栈 (audio 4K + main 6K)    | 10 KB          |
| UI framebuffer + 其它静态缓冲          | ~5 KB          |
| **小计已分配**                         | **214–224 KB** |
| **预计空闲堆**                         | **95–105 KB**  |

`*` mbedTLS 一档：本版关掉了根证书 bundle、关掉了时间相关 X.509 校验，
并通过 `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` 让 esp_tls 把 mbedTLS
authmode 设成 `MBEDTLS_SSL_VERIFY_NONE`，**完全不解析证书链、不查
签名、不查吊销**，所以握手期堆占用比常规校验少 ~10–15 KB。详见
[「TLS 安全选项」](#tls-安全选项---默认完全不校验) 一节。

切到 SoftAP 配网模式时会停止 STA 连接，主循环暂停语音、chime 和自动联网任务，只保留单按钮长按检测与 HTTP 配网页服务。

### 还能放大吗？

- **可以再放大 mic/spk arena**：受限于 power-of-two，下一档是
  **128 KB**（≈ 4 s mic 缓冲）。空闲堆会从 ~100 KB 掉到 ~35 KB，
  TLS 握手在某些证书链上瞬时分配可能会蹭到这条线，存在握手失败的
  风险。板载 8 MB PSRAM 已启用；若后续把 arena 改为动态 capability
  分配，可将大缓冲移入 PSRAM，但 I2S DMA 缓冲必须保留在内部 SRAM。
- **I2S DMA 描述符已经放大**到 mic 6×512 frame（12 KB DMA / 96 ms 采集
  窗口）和 spk 8×512 frame（8 KB DMA / 256 ms 播放窗口）。这意味着即使
  audio task 因为 SPI flash 操作或缓存抖动被挤掉 50–100 ms，DMA 也不会
  溢出/欠运行。继续放大需要消耗 `MALLOC_CAP_DMA` 池（ESP32 总共约
  110 KB，要给 Wi-Fi DMA 留 30–40 KB），所以默认到此为止；如果实测
  仍有偶发 underrun，下一步是把 spk 改成 12×512 frame（12 KB DMA /
  384 ms 窗口）。
- **本版默认值的实测意义**：
  - mic 端：可吸收最长 ~2 s 的网络回退/卡顿不丢帧；
  - spk 端：开播前先攒 0.5 s，TLS 偶发的 200–300 ms 长 read 不会
    立即引起 underrun。

如果你确认设备运行环境里堆有富余，把 `APP_AUDIO_ARENA_BYTES` 改成
`(128 * 1024)` 即可；别忘了在 `voice_chat.c` 把 `PLAYBACK_PREFILL_BYTES`
也按比例放大（保持 ≤ arena 的 1/4 比较安全）。

### 如何用日志验证当前预算合不合理

代码里在所有关键事件点都打了一行 `mem_log()`（实现见 `main/mem_log.[ch]`），
日志格式：

```
I (12345) mem: <tag> heap=NNN min=NNN int=NNN dma=NNN dma_max=NNN
```

| 字段 | 含义 |
|---|---|
| `heap`    | 当前空闲堆（DRAM 可分配总量）|
| `min`     | **启动到现在的历史最低空闲堆** —— 决定能不能再放大 buffer 的关键指标 |
| `int`     | `MALLOC_CAP_INTERNAL` 当前空闲（排除 PSRAM）|
| `dma`     | `MALLOC_CAP_DMA` 当前空闲（Wi-Fi 与 I2S DMA 共享池）|
| `dma_max` | `MALLOC_CAP_DMA` 最大连续可分配块 —— 碎片化指标 |

打点时机：

| tag                | 触发场景                          |
|--------------------|-----------------------------------|
| `boot done`        | 启动流程结束                      |
| `audio_io_init`    | audio task 创建完，arena 已占用    |
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

1. 长跑 `periodic` 的 `min` 持续下滑 → 有泄漏，不要再放大 buffer。
2. `voice tls post` 的 `min` 比 `voice tls pre` 掉 30 KB 以上 → TLS 握手
   是当前内存峰值，再放大 arena 风险偏高。
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
| 单按钮（低电平有效）       | GPIO 4         |
| 摇杆按钮 B / X / Y（低电平有效） | GPIO 19/20/21 |
| DS3231 I2C SDA / SCL       | GPIO 8/9       |
| ST7789 SCL / SDA / DC / CS | GPIO 11/12/13/14 |
| 麦克风 BCLK / LRCK / SD    | GPIO 2/1/42    |
| 扬声器 BCLK / LRC / DIN    | GPIO 41/40/39  |
| 板载 RGB LED               | GPIO 48（启动时关闭） |

GPIO39–42 同时是默认 JTAG 信号脚；当前固件将其复用为 I2S，因此需要调试时
请使用板载 USB Serial/JTAG，而不要连接外部 JTAG 到这些引脚。

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

- B / X：移动到下一项 / 上一项。
- Y 或主按钮短按：确认当前焦点。
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

第一次启动时 SPIFFS 会自动格式化。没有有效 Wi-Fi 配置或连接失败时，
设备会自动启动 SoftAP：

- SSID: `talkingflower`
- Password: `pangmiaomiao`
- URL: `http://192.168.4.1/`

TFT 会停留在配置页，显示热点名、密码和 URL。网页配置页分成 4 个独立保存按钮：Wi-Fi name + Wi-Fi password、Doubao ASR API key、Doubao Agent API key、Doubao TTS API key，可以单独更新其中一项。保存后长按 GPIO4 按钮，设备关闭 AP，按保存的 Wi-Fi 配置尝试联网；连接失败时会自动重新开启 AP，并显示 ESP-IDF disconnect reason，例如 `no AP found (201)`、`auth failed (202)`、`handshake timeout (204)`。

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
    ├── audio_io.[ch]       Core 1 上的 I2S 任务，复用 64 KB arena
    ├── voice_chat.[ch]     esp-tls chunked POST + body 直推 ring 播放
    ├── wifi_creds.[ch]     SPIFFS /wifi_config.txt 读写
    ├── app_secret_store.[ch] SPIFFS /app_secrets.txt 读写
    ├── wifi_time.[ch]      esp_wifi STA + esp_http_client 取时
    ├── wifi_provision.[ch] SoftAP + HTTP 配网页
    ├── buttons.[ch]        主按钮和 B/X/Y 输入 + 软件去抖
    ├── gui_app.h           App 描述符、输入和动作接口
    ├── gui_shell.[ch]      主导航、焦点路由和底部状态栏
    ├── app_chat_alarm.[ch] 闲聊闹钟 App
    ├── app_settings.[ch]   设置 App
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
