#include "chime_player.h"
#include "audio_io.h"
#include "doubao_agent_client.h"
#include "doubao_tts_player.h"
#include "app_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "chime";

#define CHIME_TASK_CORE 0
#define CHIME_TASK_PRIO 4

static TaskHandle_t s_worker_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static bool s_cancelled;
static volatile chime_status_t s_status = CHIME_IDLE;
static char s_msg[48] = "chime idle";
static char s_time_arg[16];
static char s_fixed_text[256];
static bool s_text_only;
static chime_effect_t s_effect;
static chime_player_status_cb_t s_status_cb;
static void *s_status_ctx;
static chime_player_text_cb_t s_text_cb;
static void *s_text_ctx;

static bool cancelled(void *ctx) {
    (void)ctx;
    if (audio_io_is_suspended()) {
        __atomic_store_n(&s_cancelled, true, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&s_cancelled, __ATOMIC_ACQUIRE);
}

void chime_player_cancel(void) {
    taskENTER_CRITICAL(&s_lock);
    bool busy = __atomic_load_n(&s_busy, __ATOMIC_RELAXED);
    if (busy) {
        __atomic_store_n(&s_cancelled, true, __ATOMIC_RELEASE);
        audio_io_request_abort();
    }
    taskEXIT_CRITICAL(&s_lock);
}

static bool begin_job(void) {
    taskENTER_CRITICAL(&s_lock);
    bool allowed = !__atomic_load_n(&s_busy, __ATOMIC_RELAXED) &&
                   audio_io_acquire_owner(AUDIO_OWNER_CHIME) == ESP_OK;
    if (allowed) {
        __atomic_store_n(&s_cancelled, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
    }
    taskEXIT_CRITICAL(&s_lock);
    return allowed;
}

static void set_status(chime_status_t st, const char *msg) {
    s_status = st;
    if (msg) {
        strncpy(s_msg, msg, sizeof(s_msg) - 1);
        s_msg[sizeof(s_msg) - 1] = 0;
    }
    if (s_status_cb) s_status_cb(s_status_ctx);
}

static void chime_tts_status(void *ctx, const char *msg) {
    (void)ctx;
    if (!msg || cancelled(NULL)) return;
    if (strstr(msg, "playing")) {
        set_status(CHIME_PLAYING, "playing chime");
    } else if (strstr(msg, "drain")) {
        set_status(CHIME_DRAINING, "drain chime");
    } else {
        set_status(CHIME_WAITING_REPLY, msg);
    }
}

static esp_err_t play_effect(chime_effect_t effect) {
    const uint32_t rate = APP_VOICE_SAMPLE_RATE;
    const size_t frames = rate * (effect == CHIME_EFFECT_WATER_DROP ? 180 : 140) / 1000;
    esp_err_t err = audio_io_prepare_speaker_cancellable(rate, 1, cancelled, NULL);
    if (err != ESP_OK) return err;
    float phase = 0.0f;
    size_t produced = 0;
    while (produced < frames) {
        if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
        uint8_t *buffer = NULL;
        size_t available = audio_io_reserve_speaker(&buffer, 250);
        if (available < sizeof(int16_t)) {
            return cancelled(NULL) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        }
        size_t count = available / sizeof(int16_t);
        if (count > frames - produced) count = frames - produced;
        int16_t *samples = (int16_t *)buffer;
        for (size_t i = 0; i < count; ++i) {
            size_t frame = produced + i;
            float t = (float)frame / (float)(frames - 1);
            float frequency = effect == CHIME_EFFECT_WATER_DROP
                                  ? 2100.0f - 1500.0f * t
                                  : 450.0f + 1500.0f * t;
            float attack = fminf((float)frame / (rate * 0.004f), 1.0f);
            float envelope = attack * (1.0f - t) * (1.0f - t);
            samples[i] = (int16_t)(6500.0f * envelope *
                                   (sinf(phase) + 0.18f * sinf(2.0f * phase)));
            phase += 6.28318530718f * frequency / rate;
            if (phase >= 6.28318530718f) phase -= 6.28318530718f;
        }
        if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
        audio_io_commit_speaker(count * sizeof(*samples));
        produced += count;
    }
    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    set_status(CHIME_PLAYING, effect == CHIME_EFFECT_WATER_DROP
                                  ? "water drop" : "bubble");
    err = audio_io_start_playback_cancellable(1000, cancelled, NULL);
    if (err != ESP_OK) return err;
    set_status(CHIME_DRAINING, "drain effect");
    return audio_io_stop_playback(2000);
}

static void chime_worker(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!chime_player_busy()) continue;
        char *text = NULL;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (cancelled(NULL)) goto done;
        if (s_effect != CHIME_EFFECT_NONE) {
            err = play_effect(s_effect);
            goto done;
        }

        if (!s_text_only) {
            set_status(CHIME_WAITING_REPLY, "agent chime");
            err = doubao_agent_chime(s_time_arg, &text);
            if (cancelled(NULL) || err != ESP_OK || !text || !text[0]) {
                if (err == ESP_OK) err = ESP_FAIL;
                goto done;
            }
            if (s_text_cb && !cancelled(NULL)) s_text_cb(text, s_text_ctx);
        }
        if (cancelled(NULL)) goto done;
        set_status(CHIME_WAITING_REPLY, "tts chime");
        err = doubao_tts_play_text_cancellable(s_text_only ? s_fixed_text : text,
                                              chime_tts_status, NULL,
                                              cancelled, NULL);
done:
        free(text);
        s_text_only = false;
        s_effect = CHIME_EFFECT_NONE;
        s_fixed_text[0] = 0;
        if (cancelled(NULL)) {
            audio_io_abort(2000);
            set_status(CHIME_IDLE, "chime cancelled");
        } else if (err != ESP_OK) {
            audio_io_abort(2000);
            ESP_LOGE(TAG, "chime err=%s", esp_err_to_name(err));
            set_status(CHIME_ERROR, "chime fail");
        } else {
            set_status(CHIME_DONE, "chime done");
        }
        audio_io_release_owner(AUDIO_OWNER_CHIME);
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    }
}

esp_err_t chime_player_init(void) {
    if (s_worker_task) return ESP_OK;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(chime_worker, "chime_worker",
                                            6144, NULL, CHIME_TASK_PRIO,
                                            &s_worker_task, CHIME_TASK_CORE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "chime PSRAM task creation failed");
        return ESP_ERR_NO_MEM;
    }
    set_status(CHIME_IDLE, "chime ready");
    return ESP_OK;
}

bool chime_player_busy(void) {
    return __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE);
}

chime_status_t chime_player_status(void) { return s_status; }
const char *chime_player_message(void) { return s_msg; }

esp_err_t chime_player_start(const char *time_arg) {
    if (!s_worker_task) return ESP_ERR_INVALID_STATE;
    if (!begin_job()) return ESP_ERR_INVALID_STATE;
    if (time_arg) {
        strncpy(s_time_arg, time_arg, sizeof(s_time_arg) - 1);
        s_time_arg[sizeof(s_time_arg) - 1] = 0;
    } else {
        s_time_arg[0] = 0;
    }
    s_text_only = false;
    s_effect = CHIME_EFFECT_NONE;
    s_fixed_text[0] = 0;
    set_status(CHIME_CONNECTING, "start chime");
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

esp_err_t chime_player_start_text(const char *text) {
    if (!s_worker_task) return ESP_ERR_INVALID_STATE;
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (!begin_job()) return ESP_ERR_INVALID_STATE;
    strncpy(s_fixed_text, text, sizeof(s_fixed_text) - 1);
    s_fixed_text[sizeof(s_fixed_text) - 1] = 0;
    s_text_only = true;
    s_effect = CHIME_EFFECT_NONE;
    s_time_arg[0] = 0;
    set_status(CHIME_CONNECTING, "start tts");
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

esp_err_t chime_player_start_effect(chime_effect_t effect) {
    if (effect != CHIME_EFFECT_WATER_DROP && effect != CHIME_EFFECT_BUBBLE) {
        ESP_LOGE(TAG, "invalid local effect: %d", (int)effect);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_worker_task || !begin_job()) return ESP_ERR_INVALID_STATE;
    s_effect = effect;
    s_text_only = false;
    s_time_arg[0] = 0;
    s_fixed_text[0] = 0;
    set_status(CHIME_CONNECTING, "start local effect");
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

void chime_player_set_status_cb(chime_player_status_cb_t cb, void *ctx) {
    s_status_cb = cb;
    s_status_ctx = ctx;
}

void chime_player_set_text_cb(chime_player_text_cb_t cb, void *ctx) {
    s_text_cb = cb;
    s_text_ctx = ctx;
}
