# 主循环与音频任务状态设计

本文说明 `app_main`、语音/报时 worker 和 Core 1 `audio_io` 任务之间的状态
所有权、切换顺序和失败恢复规则。

## 任务分工

| 执行上下文 | Core | 主要职责 |
|---|---:|---|
| `app_main` | 0 | 按钮手势、录音期间泵送、RTC/UI、Wi-Fi 重连、报时调度和业务互斥 |
| `voice_worker` | 0 | 停止录音、提交 ASR 尾帧、获取 Agent 回复、同步执行 TTS 播放 |
| `chime_worker` | 0 | 获取随机/整点 Agent 文本、同步执行 TTS 播放 |
| `audio_io` | 1 | I2S 麦克风采集、32→16 kHz 转换、扬声器 DMA 写入和设备安装/卸载 |

Core 0 的网络 worker 可能因 HTTPS/WebSocket 阻塞，但 Core 1 的
`audio_io` 使用最高 FreeRTOS 任务优先级继续服务 I2S。业务层不直接操作
I2S channel，只通过 `audio_io` 请求切换阶段和读写 SPSC ring。

## 三层状态

### 主循环状态

- `s_voice_recording`：主循环是否应继续调用 `voice_chat_pump()`。
- `s_setup_mode`：配置模式下跳过语音、自动 chime、RTC 刷新和 STA 重连。
- `audio_activity_busy()`：统一检查 voice、chime 和 `audio_io_phase()`，
  音频活动期间禁止自动联网重试和网络校时。

按钮长按启动语音前还会再次检查：

```text
voice_chat_busy() == false
chime_player_busy() == false
audio_io_phase() == IDLE
```

因此语音、chime 和底层 I2S 不会并行占用共享音频资源。

### Voice/Chime 业务状态

Voice 状态：

```text
IDLE/DONE/ERROR
  -> CONNECTING -> RECORDING -> UPLOADING
  -> WAITING_REPLY -> PLAYING -> DRAINING -> DONE
```

Chime 状态：

```text
IDLE/DONE/ERROR
  -> CONNECTING -> WAITING_REPLY -> PLAYING -> DRAINING -> DONE
```

`DONE` 和 `ERROR` 是终态但不算 busy，下一次操作可以直接覆盖为新的
`CONNECTING`。状态变量为跨任务读取的 `volatile` 枚举；worker 更新状态
和消息后，通过 callback 设置主循环的 dirty 标志，由主循环统一刷新 TFT。

### `audio_io` 阶段

```text
IDLE -> REC  -> IDLE
IDLE -> PLAY -> IDLE
```

- `REC`：Core 1 是 mic ring 生产者，`app_main`/`voice_worker` 是消费者。
- `PLAY`：TTS 所在的 Core 0 worker 是 speaker ring 生产者，Core 1 是消费者。
- REC 和 PLAY 不重叠；同一块 64 KB arena 按 `MIC`/`SPK` role 复用。
- 录音开始时重置 ring 并设置为 `MIC`；TTS 连接建立后显式清空 ring 并设置
  为 `SPK`，随后才允许预填充和启动播放。
- mic API 和 speaker API 都检查 ring role，避免状态错误时交叉破坏数据。

## 语音完整时序

1. 长按达到 800 ms，主循环调用 `voice_chat_start()`。
2. Core 0 同步建立豆包 ASR 会话，再请求 Core 1 进入 `REC`。
3. `audio_io` 安装麦克风、清空 arena、确认 `REC`；启动函数收到确认后，
   Voice 才进入 `RECORDING`。
4. 主循环仅在 `s_voice_recording` 为 true 时读取 mic ring 并发送 ASR 音频。
5. 松开或达到 10 秒上限后，主循环先清除 `s_voice_recording`，再把 Voice
   切到 `UPLOADING` 并唤醒 `voice_worker`，因此主循环不会和 worker 同时
   使用 `s_io_buf` 或 ASR session。
6. worker 请求 Core 1 停止录音，等待 I2S 卸载和 `IDLE` 确认，然后排空
   mic ring 并发送最终 ASR 帧。
7. ASR 文本经过 Agent 后进入 TTS。TTS 先把 arena 切成 speaker role 并
   预填 PCM，达到 32 KB 或响应结束时请求 Core 1 进入 `PLAY`。
8. 响应结束后请求 `stop_playback()`；Core 1 排空 ring、等待 DMA 尾音、
   卸载扬声器并确认 `IDLE`，随后 Voice 才进入 `DONE`。

## Chime 时序

`chime_player_start()` 只负责保存整点参数并把状态设为 `CONNECTING`，实际
Agent 和 TTS 工作都由 `chime_worker` 完成。TTS 播放阶段与 Voice 使用同一
套 speaker role、预填充、PLAY 和排空逻辑。

主循环在启动 chime 前检查 Voice、Chime 和底层 phase，因此手动 chime、
自动随机发言和整点报时不会打断录音或已有播放。当前没有音频任务队列：
手动触发遇到 busy 会直接失败；自动调度是否重试由 chime 调度器决定。

## 超时和错误恢复

- ASR 发送失败：关闭 ASR session，停止录音；停止超时则请求底层 abort。
- 录音停止失败：请求 abort、关闭 ASR，Voice 进入 `ERROR`。
- Agent 失败：此时录音已经停止且 `audio_io` 为 `IDLE`，直接进入 `ERROR`。
- TTS HTTP/解析失败：若 PLAY 已启动，先停止播放；停止超时则 abort。
- 播放排空超时：abort 后返回失败，业务状态进入 `ERROR`。
- `audio_io` 启动确认超时：清除尚未消费的启动请求并发送 stop/abort。
  Core 1 取请求和超时取消发生竞争时，audio task 不再清除取消标志，因此
  不会出现调用方已返回失败而 I2S 继续运行的状态。
- 如果硬件驱动异常到 abort 后仍无法回到 `IDLE`，主循环的底层 phase
  检查会继续阻止新的语音、chime、联网校时和自动重连，避免共享 arena
  被再次使用。此类状态需要通过日志诊断或重启恢复。

## 已确认的关键不变量

1. 只有一个音频业务可以进入活动状态。
2. REC 与 PLAY 不同时存在。
3. ring 在任一时刻只有一个生产者和一个消费者。
4. 录音结束确认后才处理 ASR 尾帧和启动 TTS。
5. TTS 完成排空并确认底层 `IDLE` 后，业务状态才进入 `DONE`。
6. 所有正常错误路径都关闭 ASR、停止/中止 I2S，并允许后续重新操作。
7. SoftAP 配置模式不会启动或泵送任何音频业务。

## 关键代码位置

| 文件 | 重点 |
|---|---|
| `main/app_main.c` | `audio_activity_busy()`、按钮状态、`pump_voice()`、调度互斥 |
| `main/voice_chat.c` | Voice 状态机、主循环到 worker 的所有权交接 |
| `main/chime_player.c` | Chime worker 状态机 |
| `main/doubao_tts_player.c` | speaker ring 预填充、PLAY 启停和错误清理 |
| `main/audio_io.c` | Core 1 phase、启动确认、取消、ring role 和 I2S 生命周期 |
| `main/ring.c` | SPSC head/tail 与内存屏障 |
