#include "voice_chat.h"
#include "audio_io.h"
#include "doubao_agent_client.h"
#include "doubao_asr_client.h"
#include "doubao_tts_player.h"
#include "mem_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "voice";

#define VOICE_TASK_CORE        0
#define VOICE_TASK_PRIO        4
#define VOICE_IO_BUF_BYTES     2048

static volatile voice_chat_status_t s_status = VC_IDLE;
static char s_msg[48] = "voice idle";
static TaskHandle_t s_worker_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static bool s_cancelled;
static bool s_finish_requested;
static doubao_asr_session_t *s_asr;
static size_t s_uploaded_bytes;

static uint8_t s_io_buf[VOICE_IO_BUF_BYTES];

static voice_chat_status_cb_t s_status_cb;
static void *s_status_ctx;
static voice_chat_turn_cb_t s_turn_cb;
static void *s_turn_ctx;

static void voice_worker(void *arg);

static bool cancelled(void *ctx) {
    (void)ctx;
    if (audio_io_is_suspended()) {
        __atomic_store_n(&s_cancelled, true, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&s_cancelled, __ATOMIC_ACQUIRE);
}

void voice_chat_cancel(void) {
    taskENTER_CRITICAL(&s_lock);
    if (__atomic_load_n(&s_busy, __ATOMIC_RELAXED)) {
        __atomic_store_n(&s_cancelled, true, __ATOMIC_RELEASE);
        audio_io_request_abort();
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void set_status(voice_chat_status_t st, const char *m) {
    s_status = st;
    if (m) {
        strncpy(s_msg, m, sizeof(s_msg) - 1);
        s_msg[sizeof(s_msg) - 1] = 0;
    }
    if (s_status_cb) s_status_cb(s_status_ctx);
}

void voice_chat_set_status_cb(voice_chat_status_cb_t cb, void *ctx) {
    s_status_cb = cb;
    s_status_ctx = ctx;
}

void voice_chat_set_turn_cb(voice_chat_turn_cb_t cb, void *ctx) {
    s_turn_cb = cb;
    s_turn_ctx = ctx;
}

esp_err_t voice_chat_init(void) {
    esp_err_t e = audio_io_init();
    if (e != ESP_OK) return e;
    if (!s_worker_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(voice_worker, "voice_worker",
                                                6144, NULL, VOICE_TASK_PRIO,
                                                &s_worker_task, VOICE_TASK_CORE);
        if (ok != pdPASS) return ESP_FAIL;
    }
    set_status(VC_IDLE, "voice ready");
    return ESP_OK;
}

bool voice_chat_busy(void) {
    return __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE);
}

voice_chat_status_t voice_chat_status(void) { return s_status; }
const char *voice_chat_message(void) { return s_msg; }

static void close_asr(void) {
    if (s_asr) {
        doubao_asr_session_close(s_asr);
        s_asr = NULL;
    }
}

esp_err_t voice_chat_start(void) {
    if (!s_worker_task) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_lock);
    if (voice_chat_busy() || audio_io_is_suspended() ||
        audio_io_phase() != AUDIO_PHASE_IDLE) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_cancelled, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_finish_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&s_lock);
    set_status(VC_CONNECTING, "asr connect");
    xTaskNotifyGive(s_worker_task);
    return ESP_OK;
}

esp_err_t voice_chat_pump(void) {
    return s_status == VC_ERROR ? ESP_FAIL : ESP_OK;
}

static esp_err_t pump_mic(void) {
    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    size_t n = audio_io_read_mic(s_io_buf, sizeof(s_io_buf), 5);
    if (n == 0) return ESP_OK;
    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = doubao_asr_session_send_audio(s_asr, s_io_buf, n, false);
    if (err != ESP_OK) return err;
    s_uploaded_bytes += n;
    return ESP_OK;
}

static esp_err_t drain_mic_to_asr_final(void) {
    uint8_t last[VOICE_IO_BUF_BYTES];
    size_t last_len = 0;

    for (;;) {
        if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
        size_t n = audio_io_read_mic(s_io_buf, sizeof(s_io_buf), 0);
        if (n == 0) break;

        if (last_len > 0) {
            esp_err_t err = doubao_asr_session_send_audio(s_asr, last, last_len, false);
            if (err != ESP_OK) return err;
            s_uploaded_bytes += last_len;
        }
        memcpy(last, s_io_buf, n);
        last_len = n;
    }

    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = doubao_asr_session_send_audio(s_asr, last_len ? last : NULL,
                                                  last_len, true);
    if (err == ESP_OK) s_uploaded_bytes += last_len;
    return err;
}

static void voice_tts_status(void *ctx, const char *msg) {
    (void)ctx;
    if (!msg || cancelled(NULL)) return;
    if (strstr(msg, "playing")) {
        set_status(VC_PLAYING, "playing...");
    } else if (strstr(msg, "drain")) {
        set_status(VC_DRAINING, "drain spk");
    } else {
        set_status(VC_WAITING_REPLY, msg);
    }
}

static void voice_worker(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!voice_chat_busy()) continue;
        char *transcript = NULL;
        char *agent_text = NULL;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        const char *failure = "asr connect fail";
        if (cancelled(NULL)) goto done;
        s_uploaded_bytes = 0;
        err = doubao_asr_session_start(&s_asr);
        if (cancelled(NULL) || err != ESP_OK) goto done;
        failure = "rec start";
        err = audio_io_start_recording_cancellable(1000, cancelled, NULL);
        if (cancelled(NULL) || err != ESP_OK) goto done;
        set_status(VC_RECORDING, "recording...");
        while (!__atomic_load_n(&s_finish_requested, __ATOMIC_ACQUIRE) &&
               !cancelled(NULL)) {
            failure = "asr send fail";
            err = pump_mic();
            if (err != ESP_OK) goto done;
            // A ready network socket must not starve the Core-0 UI task.
            vTaskDelay(1);
        }
        if (cancelled(NULL)) goto done;
        set_status(VC_UPLOADING, "asr final...");
        failure = "rec stop fail";
        err = audio_io_stop_recording(2000);
        if (cancelled(NULL) || err != ESP_OK) goto done;
        failure = "asr final fail";
        err = drain_mic_to_asr_final();
        if (cancelled(NULL) || err != ESP_OK) goto done;
        set_status(VC_WAITING_REPLY, "asr result");
        failure = "empty transcript";
        err = doubao_asr_session_finish(s_asr, &transcript);
        close_asr();
        if (cancelled(NULL) || err != ESP_OK || !transcript || !transcript[0]) {
            if (err == ESP_OK) err = ESP_FAIL;
            goto done;
        }
        ESP_LOGI(TAG, "asr uploaded=%u transcript=%s",
                 (unsigned)s_uploaded_bytes, transcript);

        set_status(VC_WAITING_REPLY, "agent reply");
        failure = "agent fail";
        err = doubao_agent_chat(transcript, &agent_text);
        if (cancelled(NULL) || err != ESP_OK || !agent_text || !agent_text[0]) {
            if (err == ESP_OK) err = ESP_FAIL;
            goto done;
        }
        if (s_turn_cb && !cancelled(NULL)) s_turn_cb(transcript, agent_text, s_turn_ctx);

        if (cancelled(NULL)) goto done;
        set_status(VC_WAITING_REPLY, "tts connect");
        failure = "tts fail";
        err = doubao_tts_play_text_cancellable(agent_text, voice_tts_status, NULL,
                                              cancelled, NULL);
done:
        // Only this task ever owns/uses/closes the network session.
        close_asr();
        free(transcript);
        free(agent_text);
        if (cancelled(NULL) || err != ESP_OK) {
            audio_io_abort(2000);
            set_status(cancelled(NULL) ? VC_IDLE : VC_ERROR,
                       cancelled(NULL) ? "voice cancelled" : failure);
        } else {
            set_status(VC_DONE, "voice done");
        }
        mem_log("voice done");
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    }
}

esp_err_t voice_chat_stop_and_process(void) {
    if (!voice_chat_busy() || cancelled(NULL) ||
        (s_status != VC_RECORDING && s_status != VC_CONNECTING)) {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_finish_requested, true, __ATOMIC_RELEASE);
    return ESP_OK;
}
