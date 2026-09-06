# 2026-06-21 SoftAP provisioning update

## Summary

Today the ESP-IDF alarm provisioning flow was changed from BLE GATT to an
ESP32-hosted Wi-Fi setup portal.

- BLE provisioning was removed from the app and build configuration.
- BTN3 long-press now toggles SoftAP provisioning mode.
- The device AP is:
  - SSID: `talkingflower`
  - Password: `pangmiaomiao`
  - Setup URL: `http://192.168.4.1/`
- The TFT stays on the setup screen while provisioning mode is active.
- Wi-Fi connection failures now show the ESP-IDF disconnect reason text and code.
- Wi-Fi credentials and Doubao API keys are runtime configuration stored in SPIFFS.

## Button behavior

| Action | Behavior |
|---|---|
| BTN3 long-press while in normal STA mode | Stop STA Wi-Fi and start `talkingflower` SoftAP + HTTP setup server |
| BTN3 long-press while in setup mode | Stop setup AP, return to STA mode, and try connecting with saved config |
| BTN3 short press | Manual time sync, when not in setup mode |

While setup mode is active, the main loop only services BTN3 and the HTTP setup
form. RTC refresh, voice/chime work, scheduled chimes, and STA reconnect attempts
are paused so the TFT does not flash back to the clock face.

## TFT setup screen

When provisioning is active, the TFT shows:

```text
WiFi setup AP
name: talkingflower
pass: pangmiaomiao
URL:192.168.4.1
<status>
```

## HTTP setup form

`http://192.168.4.1/` serves four independent save forms:

| Save button | Fields | Stored in |
|---|---|---|
| Save WiFi | `ssid`, `password` | `/spiffs/wifi_config.txt` |
| Save ASR key | `asr_key` | `/spiffs/app_secrets.txt` |
| Save Agent key | `agent_key` | `/spiffs/app_secrets.txt` |
| Save TTS key | `tts_key` | `/spiffs/app_secrets.txt` |

This allows updating Wi-Fi, ASR, Agent, and TTS settings independently. The Wi-Fi
password may be left blank when a saved Wi-Fi password already exists.

The persisted fields are:

| Field | HTML name | Required on first setup | Stored in |
|---|---|---:|---|
| WiFi name | `ssid` | yes | `/spiffs/wifi_config.txt` |
| WiFi password | `password` | yes | `/spiffs/wifi_config.txt` |
| Doubao ASR API key | `asr_key` | yes | `/spiffs/app_secrets.txt` |
| Doubao Agent API key | `agent_key` | yes | `/spiffs/app_secrets.txt` |
| Doubao TTS API key | `tts_key` | yes | `/spiffs/app_secrets.txt` |

## SPIFFS files

`/spiffs/wifi_config.txt`:

```text
<wifi ssid>
<wifi password>
```

`/spiffs/app_secrets.txt`:

```text
<doubao asr api key>
<doubao agent api key>
<doubao tts api key>
```

The old compile-time `main/app_secrets.h` flow is no longer used. API keys are
loaded at boot by `app_secret_store.[ch]` and consumed at runtime by the ASR,
Agent, and TTS clients.

## File naming

The new runtime configuration files are named by responsibility:

| File | Responsibility |
|---|---|
| `wifi_creds.[ch]` | Persist Wi-Fi STA credentials |
| `app_secret_store.[ch]` | Persist and expose runtime Doubao API keys |
| `wifi_provision.[ch]` | SoftAP + HTTP setup portal |
| `wifi_time.[ch]` | STA connect/disconnect and time API fetch |
