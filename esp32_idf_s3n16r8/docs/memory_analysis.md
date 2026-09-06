# ESP32 Wi-Fi Alarm (esp-idf) 内存与音频缓冲分析

> 适用项目：`esp32_wifi_alarm_idf`
> 目标：录音 + 上传 + 下载 + 播放全链路流畅
> 注：所有数值为基于源码 + sdkconfig 的**静态估算**，未实际构建。精确值请在构建后用 `idf.py size / size-components / size-files` 和运行时 `esp_get_free_heap_size()` 验证。

---

## 1. Flash 存储占用（4 MB）

来自 `partitions.csv`：

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| bootloader + 分区表 | 0x0000 | 36 KB | 预留 |
| nvs | 0x9000 | 24 KB | Wi-Fi 凭据 / 校准 |
| otadata | 0xF000 | 8 KB | OTA 选择标志 |
| phy_init | 0x11000 | 4 KB | PHY 校准 |
| **factory (app)** | 0x20000 | **1920 KB** | 当前运行固件 |
| **ota_1 (app)** | 0x200000 | **1920 KB** | OTA 备份槽 |
| storage (SPIFFS) | 0x3E0000 | 124 KB | `/wifi_config.txt`、`/app_secrets.txt` 等 |

应用镜像（Wi-Fi + HTTP setup + mbedTLS + I2S + SPIFFS + LwIP，`-Os`）典型 .bin ≈ **900 – 1100 KB**，OTA 余量充裕。

---

## 2. 应用层静态变量（DRAM .bss/.data）

| 文件 | 变量 | 字节 |
|---|---|---|
| audio_io.c | `s_arena[64K]`（PCM 环形缓冲，REC/PLAY 共用） | 65 536 |
| audio_io.c | `s_i2s_read_buf[512] int32` | 2 048 |
| audio_io.c | `s_i2s_write_buf[1024] u8` | 1 024 |
| audio_io.c | `s_decimate_buf[256] int16` | 512 |
| voice_chat.c | `s_io_buf[2048]` + status | ~2 100 |
| ui_screen.c | `s_framebuffer[128*64/8]` UI 画布 | 1 024 |
| wifi_provision.c | HTTP form / 状态缓冲 | ~2 KB |
| buttons.c | 单按钮状态 | ~24 |
| app_main.c | 状态字符串 | ~72 |
| font5x7.c | 字模常量 (Flash .rodata) | ~480 |

**应用静态 RAM 总计 ≈ 74 KB**（s_arena 64 KB 是绝对大头）。

---

## 3. 系统组件 RAM 预估（ESP32 ~520 KB SRAM）

| 组件 | 静态 + 运行时 RAM |
|---|---|
| ROM/启动占用 | ~30 KB |
| FreeRTOS 内核 + IDF 基础设施 | ~20 KB |
| Wi-Fi 驱动（6 静态 RX / 16 动态 RX/TX buf） | ~50–60 KB |
| LwIP（6 sockets, 5760 B 窗口） | ~15–20 KB |
| SoftAP + HTTP setup server（仅配置模式） | ~15–30 KB |
| mbedTLS（动态 buffer，8 KB content len，握手期间峰值） | ~25–45 KB（峰值） |
| 任务栈（main 6 K + audio 4 K + voice 6 K + 系统 ~15 K） | ~32 KB |
| 应用静态 | ~74 KB |

**系统级常驻预估 ≈ 270–320 KB**。

### 运行时空闲堆估算
- 空闲态（STA Wi-Fi 已起，无 TLS）：**90 – 130 KB**
- 语音聊天 + TLS 握手峰值：**40 – 80 KB**
- 危险阈值：< 20 KB（Wi-Fi/LwIP 易丢包或断连）

---

## 4. audio_io 三块缓冲详解

### 录音路径（REC：麦克风 → 网络）

```
INMP441 (32-bit, 32 kHz, 立体声)
   ↓  I2S DMA (12 KB, 6 描述符 × 512 帧)
s_i2s_read_buf[512] int32   ← 一次 i2s_channel_read 暂存
   ↓  >>16 + L/R 平均 + 数字增益 + 限幅 + 2:1 抽取
s_decimate_buf[256] int16   ← 16 kHz / 16-bit PCM 小块
   ↓  ring_write
s_arena[64 KB]（标记为 MIC，作 mic ring）
   ↓  audio_io_read_mic()
app_main / voice_worker → 豆包 ASR WebSocket
```

- `s_i2s_read_buf` 2 KB：与 `dma_frame_num=512` 对齐，一次吃完一个 DMA 描述符。
- `s_decimate_buf` 512 B：纯 scratch；2:1 抽取后正好 256 样本。
- `s_arena` 64 KB：作为单生产者/单消费者 ring，缓冲约 **2 秒** 16 kHz/16-bit PCM。Ring 满时丢整 chunk，`s_mic_overflow_chunks++`。

### 播放路径（PLAY：网络 → 喇叭）

```
豆包 TTS HTTPS 回调（Core 0 worker）
   ↓  audio_io_write_speaker → ring_write
s_arena[64 KB]（标记为 SPK，作 spk ring）
   ↓  ring_read（audio_io 任务）
s_i2s_write_buf[1024] u8     ← 一次 i2s_channel_write 暂存
   ↓  I2S DMA (8 KB, 8 描述符 × 512 帧)
MAX98357A → 喇叭
```

- `s_i2s_write_buf` 1 KB：512 帧 × 2 B，对齐 `dma_frame_num=512`，纯透传。
- `s_arena` 在 PLAY 阶段被复用：约 **2 秒** 播放缓冲。
- I2S spk DMA 另留 8 KB ≈ **256 ms** 二级缓冲。

### 设计要点
- 同一时刻只 REC 或 PLAY，`s_arena` 64 KB 复用，省一半 RAM。
- ring role 由 `audio_io` 校验；录音 API 不能读取 speaker ring，播放
  API 不能写入 mic ring。
- 三个 scratch buffer 共 3.5 KB 都是 `static` → 留在 DRAM .bss，避免撑爆 audio 任务 4 KB 栈。
- ring 本身固定分配、无碎片；网络协议和 base64 解码仍包含必要的数据搬运。

---

## 5. 调优清单（按性价比排）

> 关键参考：16 kHz × 16-bit 单声道 = **32 KB/s**；1 KB ≈ 31 ms 音频。
> 判断瓶颈：`s_mic_overflow_chunks`（录侧）/ `s_spk_underrun`（放侧）。

### 5.1 `TLS_RX_BUF_BYTES`（voice_chat.c） ★★★

- 当前 **3072** → 建议 **8192**（与 `MBEDTLS_SSL_MAX_CONTENT_LEN` 对齐）
- 作用：上传/下载共用，单次 TLS read 吃完整条 TLS 记录，减少解密 syscall ~10–20% CPU
- 代价：+5 KB DRAM

### 5.2 `PLAYBACK_PREFILL_BYTES`（voice_chat.c） ★★★

- 当前 **32 KB（~1 s）** → 建议 **48–56 KB（1.5–1.75 s）**
- 作用：开播前先缓冲；越大越扛网络抖动
- 上限：≤ `s_arena - 8 KB`，所以最多 56 KB
- 代价：开播延迟变大；不增加 RAM

### 5.3 `APP_AUDIO_ARENA_BYTES`（app_config.h） ★★

- 当前 **64 KB（~2 s）** → 建议 **96 KB（3 s）或 128 KB（4 s）**
- 作用：同时改善录（更扛 Wi-Fi 阻塞）和放（可配合更大预填）
- 代价：每加 32 KB 直接 -32 KB DRAM；建议保持 mbedTLS 握手峰值后仍 ≥ 30 KB 空闲堆

### 5.4 I2S DMA 描述符（audio_io.c） ★★

| 通道 | 当前 | 建议 |
|---|---|---|
| mic | `dma_desc_num=6, frame=512` = 12 KB ≈ 96 ms | 8 × 512 = 16 KB ≈ 128 ms |
| **spk** | `dma_desc_num=8, frame=512` = 8 KB ≈ 256 ms | 12 × 512 = 12 KB ≈ 384 ms（或 16 × 512 = 16 KB ≈ 512 ms） |

- 纯硬件级 jitter buffer，audio 任务被卡 200+ ms 也不破音
- 代价：每加 8 KB DMA 少 8 KB 堆

### 5.5 Wi-Fi 缓冲（sdkconfig.defaults） ★★

```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM   6  → 10~16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM  16 → 32
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM  16 → 32
```
- 弱信号/丢包重传更顺；间接降 underrun
- 静态 RX 每个 ~1.6 KB；6→10 约 +6.4 KB

### 5.6 LwIP TCP 窗口（sdkconfig.defaults） ★★

```
CONFIG_LWIP_TCP_SND_BUF_DEFAULT  5760  → 11520 或 16384
CONFIG_LWIP_TCP_WND_DEFAULT      5760  → 11520 或 16384
```
- 当前 RTT=100 ms 单连接吞吐上限 ≈ 57 KB/s（实时音频 32 KB/s，余量小）
- 翻倍后吞吐 ≈ 115 KB/s；代价每方向 +~6 KB

### 5.7 mbedTLS 记录大小

- `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=8192`，已经够大
- 加到 16384 会多 ~8 KB 堆峰值，性价比低

### 5.8 任务栈

- `voice_worker` 6144、`audio_io` 4096，目前够
- 若后续加 JSON/header 解析，把 voice_worker 提到 **8192**
- audio_io 不要动（热点 buf 都是 static）

---

## 6. 推荐组合方案

### 方案 A — 治"录音丢字"（mic overflow 多）
1. `TLS_RX_BUF_BYTES` 3072 → **8192**
2. Wi-Fi 静态 RX 6 → **10**
3. `APP_AUDIO_ARENA_BYTES` 64 K → **96 K**

总增量 ≈ **+40 KB DRAM**

### 方案 B — 治"播放卡顿/爆音"（spk underrun 多）
1. spk DMA `dma_desc_num` 8 → **12**（+4 KB）
2. `TLS_RX_BUF_BYTES` 3072 → **8192**（+5 KB）
3. `PLAYBACK_PREFILL_BYTES` 32 K → **48 K**（不加 RAM）
4. LwIP TCP 窗口 5760 → **11520**（+~12 KB）

总增量 ≈ **+21 KB DRAM**

### 方案 C — 一次到位
A + B 并集，arena 提到 **96 KB**，总增量 ≈ **+50–55 KB**；剩余堆 40–80 KB，仍能跑但偏紧。
**强烈建议**：加完后用 `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` 在 PLAYING 阶段实测。

---

## 7. 不该改的地方

- `s_i2s_read_buf` / `s_decimate_buf` / `s_i2s_write_buf`：必须与 `dma_frame_num` 等大。要加请改 DMA 描述符数。
- `MBEDTLS_SSL_MAX_CONTENT_LEN`：再加握手峰值堆会涨。
- `voice` 任务栈过大（>12 KB）无意义，热点不在栈。

---

## 8. 后续验证步骤

构建后跑：

```powershell
idf.py size
idf.py size-components
idf.py size-files
```

运行时在 `voice_chat.c` 关键节点加日志：

```c
ESP_LOGI(TAG, "free=%u min=%u internal_free=%u",
    (unsigned)esp_get_free_heap_size(),
    (unsigned)esp_get_minimum_free_heap_size(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
```

关键采样点：boot → wifi-up → setup-ap → tls-connect → recording → uploading → playing → idle。
