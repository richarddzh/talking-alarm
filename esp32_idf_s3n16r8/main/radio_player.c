#include "radio_player.h"

#include "audio_io.h"
#include "app_config.h"
#include "mem_log.h"
#include "radio_stations.h"
#include "ring.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#define RADIO_TASK_STACK_BYTES (24 * 1024)
#define RADIO_TASK_PRIORITY 4
#define RADIO_TASK_CORE 0
#define RADIO_INPUT_BYTES 2048
#define RADIO_OUTPUT_BYTES 8192
#define RADIO_PREFILL_MS 750
#define RADIO_NETWORK_BYTES (32 * 1024)
#define RADIO_NETWORK_PREFILL_BYTES (12 * 1024)
#define RADIO_READER_STACK_BYTES (8 * 1024)
#define RADIO_HTTP_TIMEOUT_MS 1500
#define RADIO_STALL_TIMEOUT_MS 8000
#define RADIO_MAX_REDIRECTS 4
#define RADIO_MAX_RETRIES 3
#define RADIO_WRITE_TIMEOUT_MS 2000
#define RADIO_SPECTRUM_INTERVAL_MS 80
#define RADIO_SPECTRUM_SAMPLES 128
#define RADIO_TWO_PI 6.28318530717958647692f

static const char *TAG = "radio_player";

static TaskHandle_t s_task;
static bool s_stop_requested;
static bool s_busy;
static radio_player_snapshot_t s_snapshot = {
    .state = RADIO_PLAYER_IDLE,
    .station_index = 0,
    .volume_level = 3,
    .message = "未播放",
};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_spectrum_ms;
static float s_spectrum_peak;

typedef struct {
    esp_http_client_handle_t client;
    ring_t ring;
    bool reader_stop;
    bool reader_done;
    esp_err_t reader_result;
    esp_err_t header_result;
    size_t metadata_interval;
    size_t audio_remaining;
    size_t metadata_remaining;
} radio_stream_t;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static bool stop_requested(void) {
    if (audio_io_is_suspended()) {
        __atomic_store_n(&s_stop_requested, true, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE);
}

static bool playback_cancelled(void *ctx) {
    (void)ctx;
    return stop_requested();
}

static void set_state(radio_player_state_t state, const char *message) {
    taskENTER_CRITICAL(&s_lock);
    if (stop_requested() && state != RADIO_PLAYER_IDLE) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    s_snapshot.state = state;
    snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s",
             message ? message : "");
    ++s_snapshot.generation;
    taskEXIT_CRITICAL(&s_lock);
}

static void set_format(const esp_audio_simple_dec_info_t *info) {
    taskENTER_CRITICAL(&s_lock);
    s_snapshot.sample_rate = info->sample_rate;
    s_snapshot.channels = info->channel;
    s_snapshot.bitrate = info->bitrate;
    ++s_snapshot.generation;
    taskEXIT_CRITICAL(&s_lock);
}

static void finish_task(void) {
    audio_io_release_owner(AUDIO_OWNER_RADIO);
    taskENTER_CRITICAL(&s_lock);
    s_task = NULL;
    ++s_snapshot.generation;
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&s_lock);
    vTaskDeleteWithCaps(NULL);
}

static void process_pcm(int16_t *samples, size_t size, uint8_t channels) {
    static const uint8_t gain_percent[] = {20, 40, 60, 80, 100};
    uint8_t volume;
    taskENTER_CRITICAL(&s_lock);
    volume = s_snapshot.volume_level;
    taskEXIT_CRITICAL(&s_lock);

    size_t sample_count = size / sizeof(*samples);
    int gain = gain_percent[volume - RADIO_VOLUME_MIN];
    if (gain < 100) {
        for (size_t i = 0; i < sample_count; ++i) {
            samples[i] = (int16_t)(((int32_t)samples[i] * gain) / 100);
        }
    }

    int64_t current_ms = now_ms();
    size_t frame_count = sample_count / channels;
    if (current_ms - s_last_spectrum_ms < RADIO_SPECTRUM_INTERVAL_MS ||
        frame_count < RADIO_SPECTRUM_SAMPLES) {
        return;
    }
    s_last_spectrum_ms = current_ms;

    static const uint8_t spectrum_bins[RADIO_SPECTRUM_BANDS] = {
        1, 2, 3, 4, 5, 7, 9, 12, 16, 22,
    };
    float values[RADIO_SPECTRUM_BANDS];
    float frame_peak = 0.0f;
    for (size_t band = 0; band < RADIO_SPECTRUM_BANDS; ++band) {
        float coefficient =
            2.0f * cosf(RADIO_TWO_PI * spectrum_bins[band] /
                        RADIO_SPECTRUM_SAMPLES);
        float q0 = 0.0f;
        float q1 = 0.0f;
        float q2 = 0.0f;
        for (size_t frame = 0; frame < RADIO_SPECTRUM_SAMPLES; ++frame) {
            int32_t mono = samples[frame * channels];
            if (channels == 2) {
                mono = (mono + samples[frame * channels + 1]) / 2;
            }
            q0 = (float)mono + coefficient * q1 - q2;
            q2 = q1;
            q1 = q0;
        }
        float power = q1 * q1 + q2 * q2 - coefficient * q1 * q2;
        values[band] = sqrtf(fmaxf(power, 0.0f)) /
                       RADIO_SPECTRUM_SAMPLES;
        if (values[band] > frame_peak) frame_peak = values[band];
    }

    s_spectrum_peak = fmaxf(frame_peak, s_spectrum_peak * 0.85f);
    taskENTER_CRITICAL(&s_lock);
    for (size_t band = 0; band < RADIO_SPECTRUM_BANDS; ++band) {
        int level = 0;
        if (s_spectrum_peak >= 80.0f) {
            float ratio = values[band] / s_spectrum_peak;
            level = (int)(sqrtf(fminf(ratio, 1.0f)) * 100.0f);
        }
        int previous = s_snapshot.spectrum[band];
        int smoothed = level > previous
                           ? (previous + level * 2) / 3
                           : previous > 8 ? previous - 8 : 0;
        s_snapshot.spectrum[band] = (uint8_t)smoothed;
    }
    ++s_snapshot.generation;
    taskEXIT_CRITICAL(&s_lock);
}

static esp_err_t push_pcm(uint8_t *data, size_t size, uint8_t channels,
                          bool *playback_started, size_t *prefill_bytes,
                          size_t prefill_target) {
    process_pcm((int16_t *)data, size, channels);
    size_t offset = 0;
    while (offset < size && !stop_requested()) {
        size_t written = audio_io_write_speaker(
            data + offset, size - offset, RADIO_WRITE_TIMEOUT_MS);
        if (written == 0) return ESP_ERR_TIMEOUT;
        if (stop_requested()) return ESP_ERR_INVALID_STATE;
        offset += written;
        if (!*playback_started) {
            *prefill_bytes += written;
            if (*prefill_bytes >= prefill_target) {
                esp_err_t err = audio_io_start_playback_cancellable(2000, playback_cancelled, NULL);
                if (err != ESP_OK) return err;
                *playback_started = true;
                set_state(RADIO_PLAYER_PLAYING, "正在播放");
            }
        }
    }
    return stop_requested() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event) {
    radio_stream_t *stream = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(event->header_key, "icy-metaint") == 0) {
        char *end = NULL;
        unsigned long interval = strtoul(event->header_value, &end, 10);
        if (end == event->header_value || *end != '\0' ||
            interval == 0 || interval > 1024 * 1024) {
            stream->header_result = ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG, "invalid icy-metaint header");
        } else {
            stream->metadata_interval = interval;
            stream->audio_remaining = interval;
        }
    }
    return ESP_OK;
}

static size_t strip_metadata(radio_stream_t *stream, uint8_t *data, size_t size) {
    if (stream->metadata_interval == 0) return size;
    size_t audio_size = 0;
    for (size_t i = 0; i < size; ++i) {
        if (stream->metadata_remaining > 0) {
            if (--stream->metadata_remaining == 0) {
                stream->audio_remaining = stream->metadata_interval;
            }
        } else if (stream->audio_remaining == 0) {
            stream->metadata_remaining = (size_t)data[i] * 16;
            if (stream->metadata_remaining == 0) {
                stream->audio_remaining = stream->metadata_interval;
            }
        } else {
            data[audio_size++] = data[i];
            --stream->audio_remaining;
        }
    }
    return audio_size;
}

static void stream_reader(void *arg) {
    radio_stream_t *stream = arg;
    esp_err_t result = ESP_OK;
    int64_t last_data_ms = now_ms();
    while (!stop_requested() &&
           !__atomic_load_n(&stream->reader_stop, __ATOMIC_ACQUIRE)) {
        uint8_t *buffer = NULL;
        size_t capacity = ring_write_acquire(&stream->ring, &buffer);
        if (capacity == 0) {
            last_data_ms = now_ms();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (capacity > RADIO_INPUT_BYTES) capacity = RADIO_INPUT_BYTES;
        int read = esp_http_client_read(stream->client, (char *)buffer,
                                       (int)capacity);
        if (read == -ESP_ERR_HTTP_EAGAIN) {
            if (now_ms() - last_data_ms >= RADIO_STALL_TIMEOUT_MS) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (read <= 0) {
            ESP_LOGW(TAG, "HTTP stream ended read=%d errno=%d", read,
                     esp_http_client_get_errno(stream->client));
            result = read == 0 ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
            break;
        }
        last_data_ms = now_ms();
        ring_write_commit(&stream->ring,
                          strip_metadata(stream, buffer, (size_t)read));
    }
    stream->reader_result = result;
    __atomic_store_n(&stream->reader_done, true, __ATOMIC_RELEASE);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t decode_stream(radio_stream_t *stream) {
    uint8_t *network = heap_caps_malloc(RADIO_NETWORK_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *input = heap_caps_malloc(RADIO_INPUT_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *output = heap_caps_malloc(RADIO_OUTPUT_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!network || !input || !output) {
        free(network);
        free(input);
        free(output);
        return ESP_ERR_NO_MEM;
    }
    ring_init(&stream->ring, network, RADIO_NETWORK_BYTES);

    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_cfg_t decoder_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_err_t decoder_err =
        esp_audio_simple_dec_open(&decoder_cfg, &decoder);
    if (decoder_err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed: %d", decoder_err);
        free(network);
        free(input);
        free(output);
        return ESP_FAIL;
    }

    bool format_ready = false;
    bool playback_started = false;
    bool network_prefilled = false;
    size_t prefill_bytes = 0;
    size_t prefill_target = 0;
    uint8_t channels = 0;
    uint32_t underruns = 0;
    esp_err_t result = ESP_OK;
    set_state(RADIO_PLAYER_BUFFERING, "正在缓冲");

    if (xTaskCreatePinnedToCoreWithCaps(stream_reader, "radio_http",
                               RADIO_READER_STACK_BYTES, stream,
                               RADIO_TASK_PRIORITY, NULL,
                               RADIO_TASK_CORE,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        esp_audio_simple_dec_close(decoder);
        free(network);
        free(input);
        free(output);
        return ESP_ERR_NO_MEM;
    }

    while (!stop_requested()) {
        bool reader_done =
            __atomic_load_n(&stream->reader_done, __ATOMIC_ACQUIRE);
        size_t available = ring_available(&stream->ring);
        if ((!network_prefilled && available < RADIO_NETWORK_PREFILL_BYTES &&
             !reader_done) || available == 0) {
            if (reader_done && available == 0) {
                result = stream->reader_result;
                break;
            }
            if (playback_started && audio_io_spk_underrun() != underruns) {
                ESP_LOGW(TAG, "network buffer exhausted; rebuffering");
                result = audio_io_stop_playback(2000);
                if (result != ESP_OK) break;
                playback_started = false;
                network_prefilled = false;
                prefill_bytes = 0;
                set_state(RADIO_PLAYER_BUFFERING, "网络慢，重新缓冲");
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        network_prefilled = true;
        size_t read = ring_read(&stream->ring, input, RADIO_INPUT_BYTES);
        esp_audio_simple_dec_raw_t raw = {
            .buffer = input,
            .len = (uint32_t)read,
            .eos = false,
        };
        while (raw.len > 0 && !stop_requested()) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = output,
                .len = RADIO_OUTPUT_BYTES,
            };
            decoder_err = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (decoder_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGE(TAG, "decoder output needs %u bytes",
                         (unsigned)frame.needed_size);
                result = ESP_ERR_NO_MEM;
                break;
            }
            if (decoder_err != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "MP3 decode failed: %d", decoder_err);
                result = decoder_err == ESP_AUDIO_ERR_NOT_SUPPORT
                             ? ESP_ERR_NOT_SUPPORTED
                         : decoder_err == ESP_AUDIO_ERR_MEM_LACK
                             ? ESP_ERR_NO_MEM : ESP_FAIL;
                break;
            }
            if (frame.decoded_size > 0) {
                if (!format_ready) {
                    esp_audio_simple_dec_info_t info = {0};
                    decoder_err =
                        esp_audio_simple_dec_get_info(decoder, &info);
                    if (decoder_err != ESP_AUDIO_ERR_OK ||
                        info.bits_per_sample != 16 ||
                        (info.channel != 1 && info.channel != 2)) {
                        ESP_LOGE(TAG, "unsupported PCM format");
                        result = ESP_ERR_NOT_SUPPORTED;
                        break;
                    }
                    result = audio_io_prepare_speaker_cancellable(info.sample_rate,
                                                      info.channel, playback_cancelled, NULL);
                    if (result != ESP_OK) break;
                    set_format(&info);
                    prefill_target = (size_t)info.sample_rate * info.channel *
                                     sizeof(int16_t) * RADIO_PREFILL_MS / 1000;
                    if (prefill_target > APP_SPEAKER_BUFFER_BYTES - RADIO_OUTPUT_BYTES) {
                        prefill_target = APP_SPEAKER_BUFFER_BYTES - RADIO_OUTPUT_BYTES;
                    }
                    mem_log("radio PCM ready");
                    channels = info.channel;
                    format_ready = true;
                }
                bool was_started = playback_started;
                result = push_pcm(frame.buffer, frame.decoded_size,
                                  channels,
                                  &playback_started, &prefill_bytes, prefill_target);
                if (result != ESP_OK) break;
                if (!was_started && playback_started) {
                    underruns = audio_io_spk_underrun();
                }
            }
            if (raw.consumed > raw.len ||
                (raw.consumed == 0 && frame.decoded_size == 0)) {
                ESP_LOGE(TAG, "decoder made no progress or consumed invalid size");
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
        if (result != ESP_OK) break;
    }

    __atomic_store_n(&stream->reader_stop, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&stream->reader_done, __ATOMIC_ACQUIRE)) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (stop_requested() || result != ESP_OK) {
        audio_io_abort(2000);
    } else if (playback_started) {
        result = audio_io_stop_playback(3000);
        if (result != ESP_OK) audio_io_abort(2000);
    }
    esp_audio_simple_dec_close(decoder);
    free(network);
    free(input);
    free(output);
    return result;
}

static esp_err_t open_stream(radio_stream_t *stream, const char *station_name,
                             bool *retryable) {
    for (int redirects = 0; redirects <= RADIO_MAX_REDIRECTS; ++redirects) {
        if (stop_requested()) return ESP_ERR_INVALID_STATE;
        stream->metadata_interval = 0;
        stream->audio_remaining = 0;
        stream->metadata_remaining = 0;
        stream->header_result = ESP_OK;
        esp_err_t result = esp_http_client_open(stream->client, 0);
        if (result != ESP_OK) return result;
        int64_t length = esp_http_client_fetch_headers(stream->client);
        if (length < 0) return ESP_ERR_HTTP_FETCH_HEADER;
        int status = esp_http_client_get_status_code(stream->client);
        ESP_LOGI(TAG, "%s HTTP %d length=%lld", station_name, status,
                 (long long)length);
        if (status == 200 || status == 206) {
            *retryable = stream->header_result == ESP_OK;
            return stream->header_result;
        }
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirects == RADIO_MAX_REDIRECTS) {
                *retryable = false;
                return ESP_ERR_INVALID_RESPONSE;
            }
            result = esp_http_client_set_redirection(stream->client);
            if (result != ESP_OK) {
                *retryable = false;
                return result;
            }
            esp_http_client_close(stream->client);
            continue;
        }
        *retryable = status == 408 || status == 429 || status >= 500;
        char message[48];
        snprintf(message, sizeof(message), "电台 HTTP %d", status);
        set_state(RADIO_PLAYER_ERROR, message);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static void radio_task(void *arg) {
    size_t station_index = (size_t)(uintptr_t)arg;
    const radio_station_t *station = radio_station_get(station_index);
    esp_err_t result = ESP_FAIL;
    if (stop_requested()) {
        set_state(RADIO_PLAYER_IDLE, "已停止");
        finish_task();
        return;
    }

    radio_stream_t stream = {0};
    esp_http_client_config_t config = {
        .url = station->url,
        .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
        .buffer_size = RADIO_INPUT_BYTES,
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
        .max_redirection_count = RADIO_MAX_REDIRECTS,
        .event_handler = http_event,
        .user_data = &stream,
    };
    for (int attempt = 0; attempt <= RADIO_MAX_RETRIES && !stop_requested();
         ++attempt) {
        stream = (radio_stream_t){0};
        stream.client = esp_http_client_init(&config);
        if (!stream.client) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        esp_http_client_set_method(stream.client, HTTP_METHOD_GET);
        esp_http_client_set_header(stream.client, "Accept", "audio/mpeg");
        esp_http_client_set_header(stream.client, "Icy-MetaData", "0");
        esp_http_client_set_header(stream.client, "User-Agent", "TalkingAlarm/1.0");
        set_state(RADIO_PLAYER_CONNECTING, "正在连接");
        bool retryable = true;
        result = open_stream(&stream, station->name, &retryable);
        if (!stop_requested() && result == ESP_OK) {
            result = decode_stream(&stream);
        }
        esp_http_client_close(stream.client);
        esp_http_client_cleanup(stream.client);
        if (stop_requested() || result == ESP_OK || !retryable ||
            result == ESP_ERR_NO_MEM || result == ESP_ERR_NOT_SUPPORTED ||
            attempt == RADIO_MAX_RETRIES) break;
        ESP_LOGW(TAG, "stream interrupted: %s; retry %d/%d",
                 esp_err_to_name(result), attempt + 1, RADIO_MAX_RETRIES);
        char message[48];
        snprintf(message, sizeof(message), "重连 %d/%d", attempt + 1,
                 RADIO_MAX_RETRIES);
        set_state(RADIO_PLAYER_CONNECTING, message);
        int64_t retry_at = now_ms() + (1000 << attempt);
        while (!stop_requested() && now_ms() < retry_at) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (stop_requested()) {
        set_state(RADIO_PLAYER_IDLE, "已停止");
    } else if (result == ESP_OK) {
        set_state(RADIO_PLAYER_IDLE, "播放结束");
    } else {
        ESP_LOGE(TAG, "stream failed: %s", esp_err_to_name(result));
        radio_player_snapshot_t snapshot;
        radio_player_get_snapshot(&snapshot);
        if (snapshot.state != RADIO_PLAYER_ERROR) {
            set_state(RADIO_PLAYER_ERROR,
                      result == ESP_ERR_NO_MEM ? "播放器内存不足"
                      : result == ESP_ERR_NOT_SUPPORTED ? "电台格式不支持"
                      : result == ESP_ERR_TIMEOUT ? "网络超时，请重试"
                      : "连接或解码失败，请重试");
        }
    }
    finish_task();
}

esp_err_t radio_player_init(void) {
    esp_audio_err_t err = esp_audio_dec_register_default();
    if (err != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    err = esp_audio_simple_dec_register_default();
    return err == ESP_AUDIO_ERR_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t radio_player_start(size_t station_index) {
    if (!radio_station_get(station_index)) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    if (radio_player_busy() ||
        audio_io_acquire_owner(AUDIO_OWNER_RADIO) != ESP_OK) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
    s_snapshot.station_index = station_index;
    s_snapshot.sample_rate = 0;
    s_snapshot.channels = 0;
    s_snapshot.bitrate = 0;
    memset(s_snapshot.spectrum, 0, sizeof(s_snapshot.spectrum));
    s_last_spectrum_ms = 0;
    s_spectrum_peak = 0.0f;
    s_snapshot.state = RADIO_PLAYER_CONNECTING;
    snprintf(s_snapshot.message, sizeof(s_snapshot.message), "准备连接");
    ++s_snapshot.generation;
    taskEXIT_CRITICAL(&s_lock);

    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        radio_task, "radio_player", RADIO_TASK_STACK_BYTES,
        (void *)(uintptr_t)station_index, RADIO_TASK_PRIORITY, &s_task,
        RADIO_TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        if (stop_requested()) set_state(RADIO_PLAYER_IDLE, "已停止");
        else set_state(RADIO_PLAYER_ERROR, "播放器任务创建失败");
        audio_io_release_owner(AUDIO_OWNER_RADIO);
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t radio_player_stop(uint32_t timeout_ms) {
    if (!radio_player_busy()) {
        if (s_snapshot.state != RADIO_PLAYER_ERROR) {
            set_state(RADIO_PLAYER_IDLE, "未播放");
        }
        return ESP_OK;
    }
    radio_player_request_stop();
    int64_t start = now_ms();
    while (radio_player_busy()) {
        if ((uint32_t)(now_ms() - start) >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool radio_player_busy(void) {
    return __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE);
}

void radio_player_request_stop(void) {
    taskENTER_CRITICAL(&s_lock);
    if (radio_player_busy()) {
        __atomic_store_n(&s_stop_requested, true, __ATOMIC_RELEASE);
        s_snapshot.state = RADIO_PLAYER_STOPPING;
        snprintf(s_snapshot.message, sizeof(s_snapshot.message), "正在停止");
        ++s_snapshot.generation;
        audio_io_request_abort();
    }
    taskEXIT_CRITICAL(&s_lock);
}

void radio_player_get_snapshot(radio_player_snapshot_t *snapshot) {
    if (!snapshot) return;
    taskENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_lock);
}

void radio_player_set_volume(uint8_t level) {
    if (level < RADIO_VOLUME_MIN) level = RADIO_VOLUME_MIN;
    if (level > RADIO_VOLUME_MAX) level = RADIO_VOLUME_MAX;
    taskENTER_CRITICAL(&s_lock);
    if (s_snapshot.volume_level != level) {
        s_snapshot.volume_level = level;
        ++s_snapshot.generation;
    }
    taskEXIT_CRITICAL(&s_lock);
}
