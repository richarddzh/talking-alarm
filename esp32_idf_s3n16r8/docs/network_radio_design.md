# 网络电台 App 设计

## 目标

网络电台 App 在 Core 0 拉取实时 MP3 流，使用 Espressif 官方
`esp_audio_codec` MP3 解码器输出 16-bit PCM，再交给 Core 1 的
`audio_io` I2S 任务播放。网络、TLS、MP3 解码和 GUI 不得进入 Core 1。

MAX98357A 是 I2S 功放，不直接接收 MP3 压缩数据。固件根据解码结果动态配置
I2S 采样率和单/双声道，不把广播固定降采样到 16 kHz。

## 电台目录

电台定义集中在 `radio_stations.c`。当前内置 24 个 MP3 直播流，覆盖中央、
山东、济南、香港、上海以及英美新闻电台。所有地址都必须能被当前 MP3
解码器直接读取；HLS/AAC 地址不加入目录。

目录顺序固定为：上海、中央、山东、济南、其他国内、香港、国外。

主要新增频道包括中国之声、山东经济/文艺/音乐广播、济南新闻/交通/音乐
广播、济南故事广播、江苏故事广播、AsiaFM 亚洲天空台、两广之声音乐台、
香港电台第一至第三台和普通话台、BBC World Service、CNN International、
NPR Program Stream 与北京新闻广播。原有五个上海频道保留。

直播地址属于第三方服务，可能调整、鉴权或停止。临时连接、断流或解码失败
只重试原电台，最多重连 3 次，分别等待 1、2、4 秒，并显示重连进度；
永久 HTTP 错误（如 403/404）、不支持的音频格式、内存不足不重试。
最终失败必须显示错误，不得静默切换到其它来源。

## 模块边界

### `radio_stations`

- 保存稳定的台名、地区和 URL；
- UI 和播放器只通过索引读取；
- 后续增加全国电台时只修改目录模块。

### `radio_player`

- Core 0 独立 HTTP reader 持续写入 32 KB PSRAM 压缩数据 ring，
  解码 worker 独立消费；PCM 输出等待不会阻塞网络读取；
- 请求 `Icy-MetaData: 0`；若服务仍返回 `icy-metaint`，跨网络分片剥离元数据，
  避免标题信息混入 MP3 字节；
- 流式 open/fetch 接口显式跟随最多 4 次 HTTP 重定向；
- 单次网络等待 1.5 秒，连续 8 秒无新数据判定为超时并进入有限重连；
- 使用 `ESP_AUDIO_SIMPLE_DEC_TYPE_MP3` 处理任意长度网络分片；
- 首帧 PCM 产生后读取采样率、声道和码率；
- 先预缓冲 12 KB MP3，再按原始格式预缓冲约 750 ms PCM 后启动 I2S；
  PCM ring 为 256 KB PSRAM（44.1 kHz 双声道约 1.49 秒），高采样率时
  预缓冲目标限制在 ring 容量内，始终为下一帧保留空间；
- 网络和 PCM 均耗尽时显示重新缓冲，重新积累数据后恢复播放，避免零星数据
  一到就短促播放；停止、退出和休眠同时取消 reader 和 decoder；
- 对解码 PCM 应用 1–5 档数字音量（20%、40%、60%、80%、100%）；
- 每 80 ms 对 128 个 PCM 帧执行 10 频带 Goertzel 能量计算；
- 频谱使用自适应峰值归一化和快速上升、慢速回落，兼顾不同电台响度；
- 暴露连接、缓冲、播放、停止、错误、音量和频谱状态快照。

### `audio_io`

- Core 1 保持最高优先级，只消费 PSRAM PCM ring，通过内部 DRAM 小缓冲写 I2S；
- I2S DMA 留在内部内存；录音/播放共享 256 KB PSRAM，
  录音使用前 64 KB，播放使用全部容量；
- `audio_io_prepare_speaker()` 在播放前设置采样率和声道；
- TTS 继续使用 16 kHz mono，电台使用 MP3 帧报告的原始格式；
- 录音、TTS、Chime 和网络电台共享统一音频所有权，不能并发。

### `app_radio`

- 电台条目使用运行时生成的纵向焦点邻接表和四行滚动窗口；
- Y 轴上下选台，X 轴左右调节 1–5 档音量；
- 主按钮或摇杆 B 播放/停止所选电台；
- 显示当前连接状态、PCM 格式和 10 柱实时频谱；
- 离开 App 自动停止网络流和 I2S 播放。

## 任务与 Core

| 工作 | Core | 优先级 |
|---|---:|---:|
| HTTP/TLS reader | 0 | 4 |
| MP3 解码、PCM 生产 | 0 | 4 |
| GUI、按钮、Wi-Fi 管理 | 0 | 默认 |
| 麦克风和扬声器 I2S | 1 | `configMAX_PRIORITIES - 1` |

MP3 worker 的 24 KB 栈、HTTP reader 的 8 KB 栈及压缩/解码数据缓冲均在
PSRAM；退出型 worker 使用 WithCaps 配对创建和删除。
reader 完全结束后才释放压缩 ring 和 HTTP client，防止停止/重连过程中访问
已释放内存。
组件配置仅保留 MP3 decoder，关闭未使用的编解码器，减少固件体积和注册开销。

mbedTLS 使用 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`，将握手和动态 TLS
接收缓冲放到 PSRAM。仅移动 PCM 缓冲还不够：TLS 若仍强制从内部 RAM
分配，接收过程中会挤占和碎片化 I2S DMA 内存，导致开播失败或 TLS 读流中断。

电台在连接前领取 `AUDIO_OWNER_RADIO`，贯穿重连和重新缓冲；HTTP reader
及 decoder 清理后才释放 owner。完整分配/扩展规则见
[memory_analysis.md](memory_analysis.md)。
