# Doubao Agent + ASR migration plan

> Historical migration record. The migration is complete; current runtime
> state and task ownership are documented in
> [`audio_state_machine.md`](audio_state_machine.md).

## Goal

Replace the ESP32 alarm project's existing `zhwebapp` voice/chime dependency with direct Volcengine Doubao APIs:

- Voice chat: button-held microphone PCM -> Doubao ASR -> Doubao Agent -> Doubao TTS -> speaker.
- Chime: button random speech, hourly time announcement, and scheduled random spontaneous speech -> Doubao Agent -> Doubao TTS -> speaker.
- Remove runtime use of `zhwebapp.azurewebsites.net` and its `/api/agent/*` endpoints.

## Pre-migration state

- `voice_chat.c` opens an HTTPS chunked upload to `APP_VOICE_API_URL`, then expects a plain-text reply from `zhwebapp`.
- `chime_player.c` posts to `APP_CHIME_API_URL`, then expects plain-text chime text from `zhwebapp`.
- `doubao_tts_player.c` already talks directly to Doubao TTS and streams returned PCM into `audio_io`.
- `audio_io.c` already records 16 kHz, 16-bit mono PCM after decimating the ICS-43432 32 kHz capture stream.
- API keys are runtime configuration stored in SPIFFS by `main/app_secret_store.[ch]`.

## Target APIs

### Doubao ASR

- URL: `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream`
- Auth headers:
  - `X-Api-Key: app_secrets_asr_api_key()`
  - `X-Api-Resource-Id: APP_DOUBAO_ASR_RESOURCE_ID`
  - `X-Api-Request-Id: <uuid>`
  - `X-Api-Sequence: -1`
- Audio metadata in the first ASR client message:
  - `format=pcm`
  - `codec=raw`
  - `rate=16000`
  - `bits=16`
  - `channel=1`
  - `language=zh-CN`
- ESP32 implementation detail: use the WebSocket binary protocol directly over `esp_tls`.
  Client WebSocket frames must be masked; ASR payload compression is set to `none` to avoid a gzip dependency.

### Doubao Agent

- URL: `https://open.feedcoopapi.com/agent_api/agent/chat/completion`
- Auth header: `Authorization: Bearer app_secrets_agent_api_key()`
- Required body:
  - `bot_id: APP_DOUBAO_AGENT_BOT_ID`
  - `stream: false`
  - `messages: [{ "role": "user", "content": "<prompt>" }]`
  - Standard mode: do not pass `model`.
- Location:
  - `province=上海市`
  - `city=上海市`
  - `district=浦东新区`
  - `town=奇妙路`
  - `longitude=121.66287360729626`
  - `latitude=31.1453210701791`
  - `knowledge` includes full address and adcode `310115`.

## Code changes

1. Add `doubao_asr_client.[ch]`.
   - Owns TLS WebSocket connect, handshake, binary frame send/read, ASR request envelope, audio frame streaming, and final transcript extraction.
   - Runs only from Core 0 callers (`app_main` / `voice_worker`), never from the Core 1 audio task.
   - Exposes:
     - `doubao_asr_session_start()`
     - `doubao_asr_session_send_audio()`
     - `doubao_asr_session_finish()`
     - `doubao_asr_session_close()`

2. Add `doubao_agent_client.[ch]`.
   - Owns Agent JSON request construction, HTTPS POST, response accumulation, and `choices[0].message.content` extraction.
   - Runs from Core 0 worker tasks together with other Wi-Fi/HTTPS/TLS work.
   - Exposes:
     - `doubao_agent_chat(prompt, out_text)`
     - `doubao_agent_chime(time_arg, out_text)`

3. Update `voice_chat.c`.
   - Replace `zhwebapp` chunked upload with Doubao ASR WebSocket streaming.
   - While BTN2 is held, drain mic PCM into ASR audio frames.
   - On release, send the final ASR frame, extract transcript, call Doubao Agent, then play Agent text with existing Doubao TTS.

4. Update `chime_player.c`.
   - Replace `APP_CHIME_API_URL` HTTP call with `doubao_agent_chime()`.
   - Keep existing trigger semantics:
     - BTN1 asks for random speech.
     - scheduler passes a time argument only for top-of-hour chime.
     - scheduler omits the time argument for random chime.

5. Update configuration.
   - Remove `APP_VOICE_API_URL`, `APP_VOICE_API_TOKEN`, `APP_CHIME_API_URL`, and `APP_CHIME_API_TOKEN`.
   - Add runtime secrets persisted by `app_secret_store.[ch]`:
     - Doubao ASR API key
     - Doubao Agent API key
     - Doubao TTS API key
   - Add non-secret defaults:
     - `APP_DOUBAO_ASR_URL`
     - `APP_DOUBAO_ASR_RESOURCE_ID`
     - `APP_DOUBAO_AGENT_URL`
     - `APP_DOUBAO_AGENT_BOT_ID`
     - Shanghai location constants.

6. Build validation.
   - Compile with ESP-IDF after adding the new source files to `main/CMakeLists.txt`.
   - Keep API keys out of source; configure them through the BTN3 SoftAP HTTP setup page.

## Non-goals

- Do not change button scheduling semantics.
- Do not add a new cloud proxy service.
- Do not commit API keys or generated build output.

## Core-affinity safety check

The design must keep blocking network work away from real-time audio I/O:

- `sdkconfig.defaults` pins `app_main` to Core 0 with `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y`.
- `sdkconfig.defaults` pins the Wi-Fi task to Core 0 with `CONFIG_ESP_WIFI_TASK_CORE_ID=0`.
- `sdkconfig.defaults` pins the lwIP TCP/IP task to Core 0 with `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`.
- `audio_io.c` pins the I2S mic/speaker task to Core 1 with `AUDIO_TASK_CORE 1`.
- `audio_io.c` gives the I2S task `configMAX_PRIORITIES - 1`, so Core 1 audio service has the highest task priority.
- `voice_chat.c` pins `voice_worker` to Core 0 with `VOICE_TASK_CORE 0`.
- `chime_player.c` pins `chime_worker` to Core 0 with `CHIME_TASK_CORE 0`.
- Doubao ASR WSS (`esp_tls_conn_*`), Doubao Agent HTTPS (`esp_http_client_perform`), Doubao TTS HTTPS, Wi-Fi time sync, and SoftAP HTTP setup all execute on Core 0 callers.
- Core 1 only installs/drives I2S, reads microphone DMA, decimates to 16 kHz PCM, drains speaker PCM, and waits for playback DMA.

Therefore a slow Wi-Fi/TLS/WSS operation can delay Core 0 producers/consumers and may eventually cause ring-buffer overflow/underrun if the network is too slow, but it does not block the Core 1 audio task from servicing I2S DMA.
