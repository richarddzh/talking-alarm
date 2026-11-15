# ESP32-S3 内存与音频资源约定

适用工程：`esp32_idf_s3n16r8`，硬件为 ESP32-S3 N16R8：
512 KB SRAM、8 MB Octal PSRAM、16 MB Flash。
本文描述当前实现，也是新增 App、音频业务和网络 worker 的接入约定。
容量中的 KB 按 1024 字节计算；运行时预算须核对实际构建配置和日志。

## 1. 内部 RAM、PSRAM 和 Flash 的边界

PSRAM 是外部 RAM，不是文件系统。大块 PCM、网络报文、文本、画面和
非实时任务栈使用 PSRAM；不要依赖普通 `malloc()` 自动选中它。
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 仍会让小分配优先使用内部堆。

| 资源 | 存放位置 | 原因 |
|---|---|---|
| I2S DMA 缓冲及描述符 | 内部 DMA-capable SRAM | 当前 ESP-IDF I2S 驱动的分配要求，不直接以 PSRAM 指针替代 |
| SPI TFT 的 640 B DMA 行缓冲 | 内部 SRAM | `st7789.c` 使用 `DMA_ATTR` |
| `audio_io` 的 4 KB 栈 | 内部 SRAM，Core 1 | 最高优先级，只服务 I2S 和小块 PCM 搬运 |
| 2 KB I2S 搬运区、512 B 降采样区 | 内部 SRAM | 不在高优先级路径上分配堆内存 |
| 任务控制块、ISR/cache-off 所需数据 | 内部 SRAM | 外部任务栈不改变 TCB 的内部内存要求 |
| 小型常驻凭据状态与配置页校验副本 | 内部 SRAM | 保留已有内部存储，临时副本显式要求 INTERNAL |
| 音频共享 arena、网络工作区、UI 帧 | PSRAM | 容量大，允许普通任务上下文访问 |
| 语音、报时、电台的网络/解码 worker 栈 | PSRAM，Core 0 | 与实时音频 I/O 隔离 |
| TLS 动态分配 | PSRAM | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` |
| 字库、图标、只读常量 | Flash | 不为只读资源复制同等大小的 RAM 副本 |

不是所有 ESP32-S3 外设 DMA 都绝对不能访问 PSRAM；支持情况取决于外设、
驱动、对齐和缓存同步要求。本工程保留当前 I2S/SPI 的内部 DMA 路径。
不得把 PSRAM 工作区直接传入未确认支持它的 DMA 或 cache-off/ISR 路径。

## 2. 当前缓冲与生命周期

| 内容 | 容量 | 分配和释放 |
|---|---:|---|
| 共享音频 arena | 256 KB PSRAM | `audio_io_init()` 分配，整个设备运行期保留 |
| REC ring 有效容量 | 64 KB | 使用共享 arena 的前 64 KB，无独立录音数组 |
| PLAY ring 有效容量 | 256 KB | 使用同一 arena 的完整容量 |
| I2S 搬运区 | 2 KB 内部 SRAM | `s_i2s_buffer` union，REC/PLAY 互斥复用 |
| 录音降采样输出 | 512 B 内部 SRAM | 与录音输入同时使用，不能简单合并 |
| Voice 上传工作区 | 2 KB PSRAM | Voice 初始化时分配，运行期保留 |
| Voice / Chime 栈 | 各 6 KB PSRAM | 常驻 worker；空闲时任务仍存在 |
| MP3 ring / 输入 / PCM 输出 | 32 / 2 / 8 KB PSRAM | 每次电台连接创建，reader 结束后释放 |
| 电台 decoder / HTTP reader 栈 | 24 / 8 KB PSRAM | 随电台任务创建、退出 |
| ASR TX / RX 工作区 | 8214 / 8193 B PSRAM | 每个 ASR session 分配一次，关闭时释放 |
| ASR session 与识别文本 | PSRAM，动态 | session 自有；最终文本转移给调用方 |
| Agent 响应区 | 8193 B PSRAM | 每个请求创建，提取回复后释放 |
| Agent/TTS 请求、转义文本和回复 | PSRAM，动态 | 使用者负责释放，不进入实时音频路径 |
| 当前帧 / 上一帧 | 各 38400 B PSRAM | UI 初始化分配，保留用于差分刷新 |
| 配网页 / 表单 | 4096 / 1600 B PSRAM | HTTP 请求结束释放，配置服务器固定在 Core 0 |
| Wi-Fi 扫描临时记录 | 最多 32 个，PSRAM | 扫描转换为 UI 结果后释放 |
| 小鱼缸模拟双缓冲及 RGB565 水域 | 108352 B PSRAM | 进入 App 分配，离开/息屏/休眠释放 |
| 番茄时钟 deadline / 状态 / 提醒指针 | 少量静态内部 RAM | 主循环更新，无新增任务、网络缓冲或 PCM arena |

明确分配的常驻 PSRAM 约 345 KB：音频 256 KB、UI 75 KB、
Voice 上传 2 KB、Voice/Chime 栈 12 KB。
电台运行再增加 74 KB 缓冲和栈；TLS、MP3 解码器内部堆、驱动和 TCB 另计。
不能把这些固定容量当成完整运行时峰值。
Wi-Fi、lwIP、HTTPD 等 SDK 任务仍由 SDK 管理其栈；本约定中的 PSRAM
worker 栈指应用自己创建的网络/解码任务。
小鱼缸和番茄时钟的原图 RGB565/alpha 常量位于 Flash；七个启动器图标
共约 47.25 KB Flash。图层和应用行为见 [aquarium_pomodoro.md](aquarium_pomodoro.md)。

`esp_audio_codec` 文档给出的 MP3 44.1 kHz 双声道参考堆占用约 28 KB，
不含栈。其内部对象的真实分配落点需要运行时跟踪，外部输入/输出已在
PSRAM 不代表解码器内部所有分配都在 PSRAM。

## 3. 音频所有权先于阶段

所有音频业务必须遵循：

```text
领取 owner
  -> 网络连接 / 解码准备
  -> REC -> 停止录音 -> 排空上传
  -> PLAY 预填充 -> 播放 -> 停止
  -> 关闭全部网络/解码资源，确认辅助 reader 已结束
  -> 释放 owner
```

- `voice_chat_start()`、`chime_player_start*()`、`radio_player_start()` 在
  网络任务开始前调用 `audio_io_acquire_owner()`。
- owner 覆盖整个业务，包括连接、Agent 等待、PCM 预填充和阶段间的 IDLE。
  `audio_io_phase() == IDLE` 不等于没有业务在占用共享内存。
- 同一把音频控制锁保证 owner 准入与阶段检查一致；领取要求没有 owner、
  没有待启动请求、phase 为 IDLE，且未暂停。
- 停止/休眠只锁存取消、请求 DMA 退出，不抢先释放 owner。
  原 worker 关闭资源后调用 `audio_io_release_owner()`，再清除 busy。
- release 本身不停止 I2S；如果 DMA 仍在退出，phase 继续阻止下一次领取。
- start/prepare/read/write/reserve/commit 入口要求已有 owner。业务只能由
  持有该 owner 的 worker 操作，不能借用别的业务的工作区。
- 新业务增加对应 `audio_io_owner_t`，创建失败和全部清理出口都要释放。
  App/main 的 busy 检查用于 UX，不替代底层准入。

REC/PLAY 同时只有一个 SPSC 生产者和消费者：

```text
REC:  Core 1 DMA -> 内部搬运/降采样 -> PSRAM ring -> Core 0 ASR
PLAY: Core 0 TTS/MP3 -> PSRAM ring -> Core 1 内部搬运 -> I2S DMA
```

仅在旧阶段停止、相关数据排空后重新初始化 ring。REC 切成 64 KB MIC；
speaker prepare 切成 256 KB SPK 并清空旧索引。取消 epoch 继续使旧写入、
未提交的预留和未完成的启动失效；唤醒不复活旧数据。

## 4. 网络工作区与所有权

ASR session 由一个 Core 0 worker 独占：

- PCM chunk 上限为 8192 B；发送时把 8 B ASR 头和 PCM 直接写入、掩码到
  可复用的 WebSocket TX 工作区，不再为每个 chunk 分配 payload/frame 副本。
- RX 只接收至多 8192 B WebSocket payload，并预留 1 B 字符串终止空间。
  接收指针是 session 借用区，外部不得释放或跨下一次接收保存。
- JSON 直接在 RX 工作区内解析，检查 body 长度后终止，不再复制整段 JSON。
- 提取的文本另有 PSRAM 存储；`doubao_asr_session_finish()` 将其转移给
  调用方并清空 session 指针。调用方 `free()`，session close 不重复释放。
- 接收帧上限约束文本规模；不存在独立的 2 KB 识别文本截断规则。
- TX/RX 不合并，避免接收 Ping 后发送 Pong 时覆盖尚需读取的 payload。

Agent 响应仍有 8192 B 上限。TTS 继续流式解析 base64 并推送 PCM，
不缓存完整音频响应。请求/转义文本完成使用后及时释放。

## 5. 新增 App 与修改规则

1. 网络、TLS、JSON、MP3、GUI 和文件操作只在 Core 0；Core 1 不做这些工作。
2. 大工作区及非实时网络对象显式使用
   `heap_caps_malloc/calloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。
   分配失败记录/返回错误，不静默回退到内部堆。
3. 非实时 worker 使用 `xTaskCreatePinnedToCoreWithCaps()` 选择 PSRAM 栈。
   退出型任务必须配对 `vTaskDeleteWithCaps()`；常驻任务不反复创建/删除。
4. 使用音频前领取 owner，全部生产者/句柄清理后释放。不要只检查 phase，
   不从 GUI/其他任务强制关闭 worker 正在使用的网络句柄。
5. 只能复用生命周期确实互斥的存储。MP3 ring 与 PCM ring、当前帧与上一帧、
   Core 0 上传工作区与 Core 1 搬运区不能合并。
6. ring 容量必须为 2 的幂，REC 容量不得超过共享 arena；当前有编译期检查。
   不能直接改成 96 KB。预填充必须小于容量，为下一帧保留空间。
7. 不凭感觉缩小任务栈。FreeRTOS canary 已启用，进一步缩栈先看高水位，
   同时考虑 TLS/协议解析调用栈及局部数组。
8. 不把 heap 总量、内部堆和 DMA 堆相加；DMA 是内部能力堆的一部分。
   文档的容量、所有权、Core 和生命周期必须随实现一同更新。

## 6. 构建与实机验证

当前录音 64 KB 在 16 kHz mono 16-bit 下约 2.05 s。
播放 256 KB 在 44.1 kHz stereo 16-bit 下约 1.49 s，在 TTS 格式下约 8.19 s。
电台另有 32 KB 压缩 ring；64 kbps 时约容纳 4.10 s MP3。
TTS 预填充 32 KB；电台 PCM 预填充目标约 750 ms，并限制在容量内。

2026-11-15 本次构建快照（ESP-IDF 5.3.1，ESP32-S3）：

| 指标 | 优化前 | 优化后 |
|---|---:|---:|
| 应用 main 静态 RAM 符号总量 | 79127 B | 9503 B |
| 全固件 `.dram0.data + .dram0.bss` | 116560 B | 46936 B |
| 常驻 Voice/Chime 栈内部 RAM | 12 KB | 0，改用 PSRAM |
| 电台 HTTP reader 栈内部 RAM | 8 KB | 0，改用 PSRAM |

静态内部 RAM 减少 69624 B（约 68 KB），栈迁移额外减少相应运行阶段的
内部堆占用；不是所有优化都体现在 ELF `.bss` 中。
同次实机启动 `int=176639 B`、`dma=169111 B`、`dma_max=79872 B`；
启动内部空闲约 172.5 KB，比优化前约 93 KB 明显增加。这不是播放峰值，
连续电台及完整语音链路仍须按下文进行实机验收。
这些是特定构建的结果，不是未来版本固定预算。仓库 sdkconfig 来自
IDF 5.3.5，当前本机 IDF 5.3.1 的生成配置可能改变 Wi-Fi TX 缓冲策略；
部署必须核对实际生成配置，不把构建器重写的无关配置提交为内存优化。

链接尺寸可使用 `idf.py size` 或工具链 `size -A` / `nm -S --size-sort`；
`dummy` 和调试段不是实际 RAM 分配，不加入堆预算。
16 MB Flash 中 factory/OTA 槽各 4 MB，2 MB 字库仍留在 Flash。

`mem_log()` 的 `heap/min` 含 PSRAM，判断内部内存风险应看 `int/dma/dma_max`。
增加大工作区或 DMA 数量后，记录连接、首帧、录音、播放和清理完成时的数据。
核对录音 overflow、播放 underrun、会话 owner 错误、内存是否在多轮结束后恢复。
实机验收包括电台连续播放/切台、ASR→Agent→TTS、Chime、取消和休眠唤醒；
少量既有主机回归不替代听音和实际网络验证。

关联文档：[音频状态机](audio_state_machine.md)、
[网络电台](network_radio_design.md)、[App GUI](modular_app_gui_design.md)。
