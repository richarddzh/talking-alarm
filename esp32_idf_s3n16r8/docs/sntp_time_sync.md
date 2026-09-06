# 标准 SNTP/NTP 校时说明

## NTP 与 SNTP 是什么

NTP（Network Time Protocol）是互联网标准时间同步协议，通常通过 UDP
123 端口从时间服务器取得 UTC 时间。完整 NTP 可以持续估算网络延迟、
时钟偏差和漂移。

SNTP（Simple Network Time Protocol）使用相同的数据格式和服务器，但
客户端算法更简单，适合 ESP32 这类嵌入式设备。本项目只需要定期取得一个
可信时间并写入 DS3231，因此使用 ESP-IDF 的 `esp_netif_sntp`。

## 当前实现

实现位于 `main/wifi_time.c`，时间服务器为：

```text
ntp.aliyun.com
```

一次校时流程如下：

1. ESP32 连接已保存的 Wi-Fi，并取得 IP 地址。
2. `esp_netif_sntp_init()` 启动 SNTP 客户端。
3. `esp_netif_sntp_sync_wait()` 最多等待 10 秒。
4. SNTP 把系统 Unix 时间更新为服务器提供的 UTC。
5. 固件设置时区字符串 `CST-8`，将 UTC 转换为中国标准时间 UTC+8。
6. 使用 `strftime()` 生成 `YYYY-MM-DD` 和 `HH:MM:SS`。
7. `rtc_ds3231_set_from_strings()` 把本地时间写入 DS3231。
8. 主循环随后每 250 ms 从 DS3231 读取时间，用于 TFT 和 chime 调度。
9. 完成本次同步后调用 `esp_netif_sntp_deinit()` 释放 SNTP 客户端资源。

日志示例：

```text
I (...) wifi_time: SNTP date=2026-08-15 time=10:04:06
I (...) rtc_ds3231: set time 2026-08-15 10:04:06
```

## 为什么改用 SNTP

旧实现通过 HTTPS 调用 timeapi HTTP 服务。实测在设备和电脑上都返回了
落后约 27 分钟的服务端时间，即使加入随机参数和 no-cache 请求头仍然
无效。这个问题来自时间服务本身，不是 TFT、ESP32 缓存或 DS3231。

SNTP 直接使用标准网络时间协议，不依赖网页 API、JSON、HTTP 缓存或 TLS
证书状态，更适合时钟设备。

## 校时频率

- 启动且 Wi-Fi 可用时执行一次；
- 此后每 24 小时执行一次，配置项为 `APP_TIME_REFRESH_MS`；
- 音频录制、Agent 请求或 TTS 播放期间推迟校时，避免网络和内存竞争；
- 平时走时由带后备电池的 DS3231 完成，不要求 ESP32 持续联网。

## 与语音、Agent 和 TTS 的关系

SNTP 校时不会触发语音，也不会调用豆包 Agent、ASR 或 TTS。两条链路完全
独立：

```text
SNTP:  Wi-Fi -> UDP/123 -> 系统时间 -> DS3231
语音:  麦克风 -> ASR -> Agent -> TTS -> 扬声器
```

主循环通过 `audio_activity_busy()` 检查三类状态：

```text
voice_chat_busy()
chime_player_busy()
audio_io_phase() != AUDIO_PHASE_IDLE
```

只要正在录音、等待 Agent、执行 TTS、播放或排空扬声器，周期校时就不会
开始，而是留到后续主循环再次尝试。主循环的调用顺序还保证：

1. 先尝试分发整点/chime；
2. chime 成功启动后立刻变为 busy；
3. 同一轮后面的 SNTP 条件再次检查 busy，因此不会与 chime 交织；
4. SNTP 执行期间 `app_main` 同步等待，按钮和调度代码不会并发启动新的
   Voice 或 Chime worker；
5. SNTP 不读写共享音频 arena，也不调用 `audio_io`。

因此当前实现不存在“校时触发语音”或“校时与 Agent/TTS 同时占用音频状态”
的路径。

需要注意：SNTP 最多同步等待 10 秒，这段时间主循环暂停按钮轮询和 TFT
常规刷新。通常服务器会在数秒内响应；若超时，按钮响应最多被延迟到等待
结束，但不会误触发或与语音交织。

## 时区处理

NTP/SNTP 只提供 UTC，不直接提供“上海时间”。固件通过：

```c
setenv("TZ", "CST-8", 1);
tzset();
localtime_r(&now, &local);
```

将 UTC 转为 UTC+8。POSIX 时区字符串的符号方向与直觉相反，因此
`CST-8` 表示 UTC 加 8 小时。中国目前不使用夏令时，所以不需要 DST 规则。

## 失败行为

| 情况 | 结果 |
|---|---|
| Wi-Fi 未连接 | 返回 `wifi down`，不修改 DS3231 |
| SNTP 初始化失败 | 返回 `sntp init` |
| 10 秒内未同步 | 返回 `sntp timeout`，不修改 DS3231 |
| 年份早于 2020 | 返回 `sntp invalid`，拒绝写入异常时间 |
| 格式化失败 | 返回 `sntp format` |

同步失败时继续使用 DS3231 中已有的有效时间。若 RTC 本身无效，则 TFT
显示错误状态，自动整点报时和随机调度不会运行。

## 准确度与安全性

- 家庭网络下，SNTP 通常能达到几十毫秒到数百毫秒级误差；本项目只显示到
  秒并按小时调度，精度足够。
- 传统 NTP/SNTP 默认不提供身份认证，局域网或上游网络中的攻击者理论上
  可以伪造时间。当前家庭时钟场景接受该风险。
- DS3231 的作用是离线保持时间，并不是网络时间权威；每天 SNTP 校时可
  修正其晶振长期漂移。

## 相关代码

| 文件 | 职责 |
|---|---|
| `main/wifi_time.c` | Wi-Fi 连接、SNTP 同步、UTC+8 转换 |
| `main/app_main.c:sync_time_from_wifi()` | 调度校时并写入 DS3231 |
| `main/rtc_ds3231.c` | DS3231 BCD 读写和有效性检查 |
| `main/app_config.h` | 24 小时校时间隔 |
