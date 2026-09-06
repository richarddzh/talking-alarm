#pragma once
// Hardware pin map and tunables for the ESP32-S3 N16R8 board.

#include <stdint.h>

// ---- Shared I2C0 bus (DS3231 RTC) -------------------------------------
#define APP_I2C_PORT          0      // I2C_NUM_0
#define APP_I2C_SDA_PIN       8
#define APP_I2C_SCL_PIN       9
#define APP_I2C_FREQ_HZ       400000

// ---- Native-resolution indexed-color UI canvas ------------------------
#define APP_UI_W               320
#define APP_UI_H               240

// ---- ST7789 TFT (320x240 over SPI2, landscape) -------------------------
#define APP_TFT_W              320
#define APP_TFT_H              240
#define APP_TFT_SCLK_PIN       11
#define APP_TFT_MOSI_PIN       12
#define APP_TFT_DC_PIN         13
#define APP_TFT_CS_PIN         14
#define APP_TFT_SPI_FREQ_HZ    40000000
#define APP_TFT_X_OFFSET       0
#define APP_TFT_Y_OFFSET       0

// ---- Buttons (active-low, internal pull-up) ----------------------------
#define APP_BUTTON_PIN             4
#define APP_BUTTON_B_PIN           19
#define APP_BUTTON_X_PIN           20
#define APP_BUTTON_Y_PIN           21
#define APP_BUTTON_DEBOUNCE_MS     30
#define APP_BUTTON_LONG_PRESS_MS   800

// ---- Onboard addressable RGB LED --------------------------------------
#define APP_ONBOARD_RGB_PIN        48

// ---- ICS-43432 mic on I2S0 --------------------------------------------
#define APP_MIC_BCLK_PIN      2
#define APP_MIC_LRCK_PIN      1
#define APP_MIC_DATA_PIN      42

// ---- MAX98357A speaker on I2S1 ----------------------------------------
#define APP_SPK_BCLK_PIN      41
#define APP_SPK_LRCK_PIN      40
#define APP_SPK_DATA_PIN      39

// ---- Audio parameters --------------------------------------------------
// ICS-43432 needs >~23 kHz to stay out of low-power mode, so capture at
// 32 kHz and 2:1 decimate to 16 kHz mono 16-bit before upload.
#define APP_VOICE_SAMPLE_RATE     16000U   // wire-format & speaker rate
#define APP_MIC_CAPTURE_RATE      32000U
#define APP_MIC_DIGITAL_GAIN      2
#define APP_VOICE_MAX_RECORD_MS   10000U

// Single shared audio arena reused as mic ring (RECORD phase) or
// speaker ring (PLAY phase). 必须是 2 的幂（SPSC ring 用掩码代替取模）。
// 64 KB ≈ 2 s mic 缓冲（16 kHz mono 16-bit）；WiFi 抖动时麦克风端可以
// 多撑约 1.5 s；播放预热 16 KB 后再启动 I2S，给 TLS 读数留头。
#define APP_AUDIO_ARENA_BYTES     (64 * 1024)

// ---- Network -----------------------------------------------------------
#define APP_WIFI_CONNECT_TIMEOUT_MS  20000U
#define APP_WIFI_RETRY_MS             5000U
// 周期性 SNTP 校时间隔。DS3231 负责平时走时，网络只作为时间权威来源。
#define APP_TIME_REFRESH_MS        86400000U   // 24 小时

// ---- Doubao endpoints ---------------------------------------------------
#define APP_DOUBAO_ASR_URL \
    "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream"
#define APP_DOUBAO_ASR_RESOURCE_ID   "volc.seedasr.sauc.duration"

#define APP_DOUBAO_AGENT_URL \
    "https://open.feedcoopapi.com/agent_api/agent/chat/completion"
#define APP_DOUBAO_AGENT_BOT_ID      "7647087216547694086"
#define APP_DOUBAO_LOCATION_ADDRESS  "上海市上海市浦东新区奇妙路310115"
#define APP_DOUBAO_LOCATION_PROVINCE "上海市"
#define APP_DOUBAO_LOCATION_CITY     "上海市"
#define APP_DOUBAO_LOCATION_DISTRICT "浦东新区"
#define APP_DOUBAO_LOCATION_TOWN     "奇妙路"
#define APP_DOUBAO_LOCATION_ADCODE   "310115"
#define APP_DOUBAO_LOCATION_LONGITUDE 121.66287360729626
#define APP_DOUBAO_LOCATION_LATITUDE  31.1453210701791

#define APP_DOUBAO_TTS_URL \
    "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#define APP_DOUBAO_TTS_RESOURCE_ID   "seed-icl-2.0"
#define APP_DOUBAO_TTS_SPEAKER       "S_yOMisdm22"
#define APP_DOUBAO_TTS_SAMPLE_RATE   APP_VOICE_SAMPLE_RATE

// ---- Storage -----------------------------------------------------------
#define APP_SPIFFS_BASE_PATH         "/spiffs"
#define APP_WIFI_CONFIG_PATH         "/spiffs/wifi_config.txt"
#define APP_SECRETS_CONFIG_PATH      "/spiffs/app_secrets.txt"

// ---- Wi-Fi provisioning AP ---------------------------------------------
#define APP_PROVISION_AP_SSID        "talkingflower"
#define APP_PROVISION_AP_PASSWORD    "pangmiaomiao"
#define APP_PROVISION_AP_CHANNEL     1
#define APP_PROVISION_AP_MAX_CONN    4
