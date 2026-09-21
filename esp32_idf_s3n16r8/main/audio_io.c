#include "audio_io.h"
#include "app_config.h"
#include "ring.h"
#include "mem_log.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>

static const char *TAG = "audio_io";

#define MIC_PORT    I2S_NUM_0
#define SPK_PORT    I2S_NUM_1
#define POLL_MS     2
// Pin audio to core 1: ESP-IDF Wi-Fi/lwIP and the default app_main task
// both live on core 0, so isolating I2S DMA work on core 1 actually
// removes contention rather than (as in the Arduino version) creating it.
#define AUDIO_TASK_CORE 1
// Give audio the highest FreeRTOS task priority so Core 1 work cannot
// preempt mic/speaker servicing. Interrupts can still run above tasks.
#define AUDIO_TASK_PRIO (configMAX_PRIORITIES - 1)

enum {
    REQ_NONE       = 0,
    REQ_START_REC  = 1,
    REQ_START_PLAY = 2,
};

enum {
    RING_ROLE_NONE = 0,
    RING_ROLE_MIC  = 1,
    RING_ROLE_SPK  = 2,
};

// --- Shared arena (mic ring during REC, speaker ring during PLAY) -------
static uint8_t  s_arena[APP_AUDIO_ARENA_BYTES] __attribute__((aligned(4)));
static ring_t   s_ring;

// --- Volatile request/state flags ---------------------------------------
static volatile uint8_t  s_phase     = AUDIO_PHASE_IDLE;
static volatile uint8_t  s_ring_role = RING_ROLE_NONE;
static volatile uint8_t  s_req_start = REQ_NONE;
static volatile uint8_t  s_req_stop  = 0;
static volatile uint8_t  s_req_abort = 0;
static volatile uint32_t s_mic_overflow_chunks = 0;
static volatile uint32_t s_spk_underrun = 0;
static volatile uint32_t s_spk_sample_rate = APP_VOICE_SAMPLE_RATE;
static volatile uint8_t  s_spk_channels = 1;
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_suspended;
static uint32_t s_epoch;
static uint32_t s_ring_epoch;

bool audio_io_is_suspended(void) {
    return __atomic_load_n(&s_suspended, __ATOMIC_ACQUIRE);
}

static bool ring_cancelled(void) {
    return audio_io_is_suspended() ||
           __atomic_load_n(&s_ring_epoch, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
}

static void finish_phase(void) {
    taskENTER_CRITICAL(&s_control_lock);
    s_req_stop = 0;
    s_req_abort = 0;
    s_phase = AUDIO_PHASE_IDLE;
    taskEXIT_CRITICAL(&s_control_lock);
}

// --- Reusable scratch buffers (kept off the task stack) -----------------
// 与下面 install_mic / install_speaker 的 dma_frame_num 对齐：每次
// i2s_channel_read 直接吃掉一整个 DMA 描述符，少做 syscall。
static int32_t s_i2s_read_buf[512];     // 2 KB mic DMA staging (512 frame * 4 B)
static int16_t s_decimate_buf[256];     // 512 B 32->16 kHz, 32->16 bit
static uint8_t s_i2s_write_buf[2048];   // 2 KB speaker staging (512 stereo frames)

// --- I2S handles --------------------------------------------------------
static i2s_chan_handle_t s_mic_chan;
static i2s_chan_handle_t s_spk_chan;
static bool              s_mic_installed;
static bool              s_spk_installed;

static TaskHandle_t      s_task;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

esp_err_t audio_io_quiet_speaker_pins(void) {
    const uint64_t pin_mask = (1ULL << APP_SPK_BCLK_PIN) |
                              (1ULL << APP_SPK_LRCK_PIN) |
                              (1ULL << APP_SPK_DATA_PIN);
    gpio_set_level(APP_SPK_BCLK_PIN, 0);
    gpio_set_level(APP_SPK_LRCK_PIN, 0);
    gpio_set_level(APP_SPK_DATA_PIN, 0);
    const gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static esp_err_t install_mic(void) {
    if (s_mic_installed) return ESP_OK;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_PORT, I2S_ROLE_MASTER);
    // mic 端: 6 * 512 frame * 4 B = 12 KB DMA, 约 96 ms 采集窗口 @ 32 kHz
    // 即使 audio task 因为 DRAM 缺页 / SPI flash 操作被挤掉 50 ms,
    // DMA 也不会溢出。
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = false;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_mic_chan);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic new_channel: %s", esp_err_to_name(err)); return err; }

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(APP_MIC_CAPTURE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_MIC_BCLK_PIN,
            .ws   = APP_MIC_LRCK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = APP_MIC_DATA_PIN,
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_mic_chan, &std);
    if (err != ESP_OK) { i2s_del_channel(s_mic_chan); s_mic_chan = NULL;
        ESP_LOGE(TAG, "mic init_std: %s", esp_err_to_name(err)); return err; }
    err = i2s_channel_enable(s_mic_chan);
    if (err != ESP_OK) { i2s_del_channel(s_mic_chan); s_mic_chan = NULL; return err; }
    s_mic_installed = true;
    mem_log("mic install");
    return ESP_OK;
}

static void uninstall_mic(void) {
    if (!s_mic_installed) return;
    i2s_channel_disable(s_mic_chan);
    i2s_del_channel(s_mic_chan);
    s_mic_chan = NULL;
    s_mic_installed = false;
}

static esp_err_t install_speaker(void) {
    if (s_spk_installed) return ESP_OK;
    uint32_t sample_rate = s_spk_sample_rate;
    uint8_t channels = s_spk_channels;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_PORT, I2S_ROLE_MASTER);
    // Eight 512-frame descriptors retain about 93 ms at 44.1 kHz stereo.
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_spk_chan, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "spk new_channel: %s", esp_err_to_name(err)); return err; }

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       channels == 2
                                                           ? I2S_SLOT_MODE_STEREO
                                                           : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_SPK_BCLK_PIN,
            .ws   = APP_SPK_LRCK_PIN,
            .dout = APP_SPK_DATA_PIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    if (channels == 1) std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_spk_chan, &std);
    if (err != ESP_OK) { i2s_del_channel(s_spk_chan); s_spk_chan = NULL;
        audio_io_quiet_speaker_pins();
        ESP_LOGE(TAG, "spk init_std: %s", esp_err_to_name(err)); return err; }
    memset(s_i2s_write_buf, 0, sizeof(s_i2s_write_buf));
    size_t silence_loaded = 0;
    // A 512-frame DMA buffer takes 32 ms at 16 kHz. Preload before
    // enabling instead of timing out waiting 20 ms for its first interrupt.
    err = i2s_channel_preload_data(s_spk_chan, s_i2s_write_buf,
                                   sizeof(s_i2s_write_buf), &silence_loaded);
    if (err != ESP_OK || silence_loaded != sizeof(s_i2s_write_buf)) {
        ESP_LOGE(TAG, "spk preload: %s bytes=%u/%u",
                 esp_err_to_name(err), (unsigned)silence_loaded,
                 (unsigned)sizeof(s_i2s_write_buf));
        i2s_del_channel(s_spk_chan);
        s_spk_chan = NULL;
        audio_io_quiet_speaker_pins();
        return err != ESP_OK ? err : ESP_FAIL;
    }
    err = i2s_channel_enable(s_spk_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spk enable: %s", esp_err_to_name(err));
        i2s_del_channel(s_spk_chan);
        s_spk_chan = NULL;
        audio_io_quiet_speaker_pins();
        return err;
    }
    s_spk_installed = true;
    mem_log("spk install");
    return ESP_OK;
}

static void uninstall_speaker(void) {
    if (!s_spk_installed) return;
    i2s_channel_disable(s_spk_chan);
    i2s_del_channel(s_spk_chan);
    s_spk_chan = NULL;
    s_spk_installed = false;
    audio_io_quiet_speaker_pins();
}

static size_t write_speaker(const uint8_t *data, size_t len) {
    size_t total = 0;
    while (total < len && !s_req_abort && !audio_io_is_suspended()) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_spk_chan, data + total, len - total,
                                          &written, 20);
        total += written;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) break;
    }
    return total;
}

// --- Recording phase: reuse arena as mic ring; producer = audio task ----
static void do_recording(void) {
    if (install_mic() != ESP_OK) {
        finish_phase();
        return;
    }
    ring_reset(&s_ring);
    s_ring_role = RING_ROLE_MIC;
    s_mic_overflow_chunks = 0;
    __sync_synchronize();
    s_phase = AUDIO_PHASE_REC;
    ESP_LOGI(TAG, "REC start");

    while (!s_req_stop && !s_req_abort && !audio_io_is_suspended()) {
        size_t bytes_read = 0;
        esp_err_t e = i2s_channel_read(s_mic_chan, s_i2s_read_buf,
                                       sizeof(s_i2s_read_buf),
                                       &bytes_read, 20);
        if (e != ESP_OK || bytes_read == 0) continue;

        size_t in_samples  = bytes_read / sizeof(int32_t);
        size_t out_samples = in_samples / 2;
        // 与 s_decimate_buf[256] 对齐 (新 DMA 配置下 i2s_channel_read 单次
        // 最多返回 512 个 32-bit 样本, 2:1 抽取后正好 256 个 16-bit 样本)。
        if (out_samples > 256) out_samples = 256;

        for (size_t i = 0; i < out_samples; ++i) {
            int32_t a = s_i2s_read_buf[2 * i] >> 16;
            int32_t b = s_i2s_read_buf[2 * i + 1] >> 16;
            int32_t mixed = ((a + b) / 2) * APP_MIC_DIGITAL_GAIN;
            if (mixed >  INT16_MAX) mixed =  INT16_MAX;
            if (mixed <  INT16_MIN) mixed =  INT16_MIN;
            s_decimate_buf[i] = (int16_t)mixed;
        }

        size_t out_bytes = out_samples * sizeof(int16_t);
        if (ring_free_space(&s_ring) < out_bytes) {
            s_mic_overflow_chunks++;
            continue;                  // drop whole chunk; preserve alignment
        }
        ring_write(&s_ring, (const uint8_t *)s_decimate_buf, out_bytes);
    }

    uninstall_mic();
    finish_phase();
    ESP_LOGI(TAG, "REC stop overflow_chunks=%u", (unsigned)s_mic_overflow_chunks);
    mem_log("REC stop");
}

// --- Playback phase: reuse arena as speaker ring; consumer = audio task -
static void do_playback(void) {
    if (install_speaker() != ESP_OK) {
        finish_phase();
        return;
    }
    s_spk_underrun = 0;
    __sync_synchronize();
    s_phase = AUDIO_PHASE_PLAY;
    ESP_LOGI(TAG, "PLAY start rate=%u channels=%u",
             (unsigned)s_spk_sample_rate, (unsigned)s_spk_channels);

    int64_t last_data_ms = now_ms();
    size_t  total_bytes = 0;

    while (!s_req_abort && !audio_io_is_suspended()) {
        size_t got = ring_read(&s_ring, s_i2s_write_buf, sizeof(s_i2s_write_buf));
        if (got > 0) {
            size_t wrote = write_speaker(s_i2s_write_buf, got);
            total_bytes += wrote;
            last_data_ms = now_ms();
            continue;
        }
        if (s_req_stop) {
            // Drain any leftover that arrived between read and stop check.
            got = ring_read(&s_ring, s_i2s_write_buf, sizeof(s_i2s_write_buf));
            if (got > 0) {
                size_t wrote = write_speaker(s_i2s_write_buf, got);
                total_bytes += wrote;
                continue;
            }
            break;
        }
        if (now_ms() - last_data_ms > 5) {
            s_spk_underrun++;
            last_data_ms = now_ms();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    if (!s_req_abort && !audio_io_is_suspended()) {
        memset(s_i2s_write_buf, 0, sizeof(s_i2s_write_buf));
        write_speaker(s_i2s_write_buf, sizeof(s_i2s_write_buf));
        // Normal completion drains DMA; cancellation must not wait for it.
        int64_t until = now_ms() + 350;
        while (!s_req_abort && !audio_io_is_suspended() && now_ms() < until) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
    }
    uninstall_speaker();
    finish_phase();
    ESP_LOGI(TAG, "PLAY stop bytes=%u underrun=%u",
             (unsigned)total_bytes, (unsigned)s_spk_underrun);
    mem_log("PLAY stop");
}

static void task_run(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "audio task running on core %d", xPortGetCoreID());
    while (1) {
        while (s_req_start == REQ_NONE) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
        taskENTER_CRITICAL(&s_control_lock);
        uint8_t r = s_req_start;
        s_req_start = REQ_NONE;
        // A timeout/abort can race with this task consuming the start
        // request. Never erase that cancellation after taking the request.
        bool cancelled = s_req_stop || s_req_abort || audio_io_is_suspended();
        taskEXIT_CRITICAL(&s_control_lock);
        if (cancelled) {
            finish_phase();
            continue;
        }
        if      (r == REQ_START_REC)  do_recording();
        else if (r == REQ_START_PLAY) do_playback();
    }
}

// --- Public API ---------------------------------------------------------

esp_err_t audio_io_init(void) {
    if (s_task) return ESP_OK;
    esp_err_t err = audio_io_quiet_speaker_pins();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker pin quiet: %s", esp_err_to_name(err));
        return err;
    }
    if (ring_init(&s_ring, s_arena, sizeof(s_arena)) != 0) {
        ESP_LOGE(TAG, "ring init failed");
        return ESP_FAIL;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(task_run, "audio_io",
                                            4096, NULL, AUDIO_TASK_PRIO, &s_task,
                                            AUDIO_TASK_CORE);
    if (ok != pdPASS) { ESP_LOGE(TAG, "task create"); return ESP_FAIL; }
    ESP_LOGI(TAG, "audio task started core=%d prio=%d arena=%u B",
             AUDIO_TASK_CORE, AUDIO_TASK_PRIO, (unsigned)sizeof(s_arena));
    mem_log("audio_io_init");
    return ESP_OK;
}

static esp_err_t wait_phase(audio_phase_t target, uint32_t timeout_ms) {
    int64_t t0 = now_ms();
    while (s_phase != target) {
        if (target != AUDIO_PHASE_IDLE &&
            (audio_io_is_suspended() || s_req_abort ||
             s_phase == AUDIO_PHASE_IDLE)) return ESP_ERR_INVALID_STATE;
        if (now_ms() - t0 > timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    return ESP_OK;
}

esp_err_t audio_io_start_recording(uint32_t ack_timeout_ms) {
    return audio_io_start_recording_cancellable(ack_timeout_ms, NULL, NULL);
}

esp_err_t audio_io_start_recording_cancellable(uint32_t ack_timeout_ms,
                                               audio_io_cancel_cb_t cancelled,
                                               void *ctx) {
    if (!s_task) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_control_lock);
    if ((cancelled && cancelled(ctx)) || audio_io_is_suspended() ||
        s_phase != AUDIO_PHASE_IDLE || s_req_start != REQ_NONE) {
        taskEXIT_CRITICAL(&s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_req_stop = 0;
    s_req_abort = 0;
    __atomic_store_n(&s_ring_epoch, __atomic_load_n(&s_epoch, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
    s_phase = AUDIO_PHASE_STARTING;
    s_req_start = REQ_START_REC;
    taskEXIT_CRITICAL(&s_control_lock);
    esp_err_t e = wait_phase(AUDIO_PHASE_REC, ack_timeout_ms);
    if (e != ESP_OK) {
        // Either the request was never picked up or it picked up but
        // failed during install. Either way, force back to IDLE so the
        // next attempt won't be rejected.
        audio_io_abort(2000);
    }
    return e;
}

esp_err_t audio_io_stop_recording(uint32_t ack_timeout_ms) {
    if (s_phase == AUDIO_PHASE_IDLE) return ESP_OK;
    if (s_phase != AUDIO_PHASE_REC && s_phase != AUDIO_PHASE_STARTING) return ESP_ERR_INVALID_STATE;
    s_req_stop = 1;
    __sync_synchronize();
    return wait_phase(AUDIO_PHASE_IDLE, ack_timeout_ms);
}

esp_err_t audio_io_start_playback(uint32_t ack_timeout_ms) {
    return audio_io_start_playback_cancellable(ack_timeout_ms, NULL, NULL);
}

esp_err_t audio_io_start_playback_cancellable(uint32_t ack_timeout_ms,
                                              audio_io_cancel_cb_t cancelled,
                                              void *ctx) {
    if (!s_task) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_control_lock);
    if ((cancelled && cancelled(ctx)) || ring_cancelled() ||
        s_phase != AUDIO_PHASE_IDLE || s_req_start != REQ_NONE ||
        s_ring_role != RING_ROLE_SPK) {
        taskEXIT_CRITICAL(&s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_req_stop = 0;
    s_req_abort = 0;
    s_phase = AUDIO_PHASE_STARTING;
    s_req_start = REQ_START_PLAY;
    taskEXIT_CRITICAL(&s_control_lock);
    esp_err_t e = wait_phase(AUDIO_PHASE_PLAY, ack_timeout_ms);
    if (e != ESP_OK) {
        audio_io_abort(2000);
    }
    return e;
}

esp_err_t audio_io_stop_playback(uint32_t ack_timeout_ms) {
    if (s_phase == AUDIO_PHASE_IDLE) return ESP_OK;
    if (s_phase != AUDIO_PHASE_PLAY && s_phase != AUDIO_PHASE_STARTING) return ESP_ERR_INVALID_STATE;
    s_req_stop = 1;
    __sync_synchronize();
    return wait_phase(AUDIO_PHASE_IDLE, ack_timeout_ms);
}

void audio_io_request_abort(void) {
    taskENTER_CRITICAL(&s_control_lock);
    __atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
    s_req_abort = 1;
    s_req_stop  = 1;
    taskEXIT_CRITICAL(&s_control_lock);
}

void audio_io_set_suspended(bool suspended) {
    taskENTER_CRITICAL(&s_control_lock);
    __atomic_store_n(&s_suspended, suspended, __ATOMIC_RELEASE);
    if (suspended) {
        __atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
        s_req_abort = 1;
        s_req_stop = 1;
    }
    taskEXIT_CRITICAL(&s_control_lock);
}

void audio_io_abort(uint32_t timeout_ms) {
    audio_io_request_abort();
    if (timeout_ms) wait_phase(AUDIO_PHASE_IDLE, timeout_ms);
}

esp_err_t audio_io_prepare_speaker(uint32_t sample_rate, uint8_t channels) {
    return audio_io_prepare_speaker_cancellable(sample_rate, channels, NULL, NULL);
}

esp_err_t audio_io_prepare_speaker_cancellable(uint32_t sample_rate, uint8_t channels,
                                               audio_io_cancel_cb_t cancelled,
                                               void *ctx) {
    if (sample_rate < 8000 || sample_rate > 96000 ||
        (channels != 1 && channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_control_lock);
    if ((cancelled && cancelled(ctx)) || audio_io_is_suspended() ||
        s_phase != AUDIO_PHASE_IDLE || s_req_start != REQ_NONE) {
        taskEXIT_CRITICAL(&s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_spk_sample_rate = sample_rate;
    s_spk_channels = channels;
    ring_reset(&s_ring);
    s_ring_role = RING_ROLE_SPK;
    __atomic_store_n(&s_ring_epoch, __atomic_load_n(&s_epoch, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

size_t audio_io_read_mic(uint8_t *dst, size_t maxlen, uint32_t timeout_ms) {
    if (ring_cancelled() || s_ring_role != RING_ROLE_MIC) return 0;
    if (timeout_ms == 0) return ring_read(&s_ring, dst, maxlen);
    int64_t t0 = now_ms();
    for (;;) {
        if (ring_cancelled()) return 0;
        size_t n = ring_read(&s_ring, dst, maxlen);
        if (n) return n;
        if ((uint32_t)(now_ms() - t0) >= timeout_ms) return 0;
        vTaskDelay(1);
    }
}

size_t audio_io_write_speaker(const uint8_t *src, size_t len, uint32_t timeout_ms) {
    if (ring_cancelled() || s_ring_role != RING_ROLE_SPK ||
        (s_phase != AUDIO_PHASE_IDLE && s_phase != AUDIO_PHASE_PLAY)) {
        return 0;
    }
    size_t total = 0;
    int64_t t0 = now_ms();
    while (total < len) {
        if (ring_cancelled()) break;
        size_t pushed = ring_write(&s_ring, src + total, len - total);
        if (pushed) { total += pushed; t0 = now_ms(); continue; }
        if (timeout_ms == 0 || (uint32_t)(now_ms() - t0) >= timeout_ms) break;
        vTaskDelay(1);
    }
    return total;
}

size_t audio_io_reserve_speaker(uint8_t **dst, uint32_t timeout_ms) {
    if (!dst) return 0;
    if (ring_cancelled() || s_ring_role != RING_ROLE_SPK ||
        (s_phase != AUDIO_PHASE_IDLE && s_phase != AUDIO_PHASE_PLAY)) {
        return 0;
    }
    int64_t t0 = now_ms();
    for (;;) {
        if (ring_cancelled()) return 0;
        size_t n = ring_write_acquire(&s_ring, dst);
        if (n) return n;
        if (ring_cancelled() || s_ring_role != RING_ROLE_SPK ||
            (s_phase != AUDIO_PHASE_IDLE && s_phase != AUDIO_PHASE_PLAY)) {
            return 0;
        }
        if (timeout_ms == 0 || (uint32_t)(now_ms() - t0) >= timeout_ms) return 0;
        vTaskDelay(1);
    }
}

void audio_io_commit_speaker(size_t n) {
    if (n == 0) return;
    if (ring_cancelled() || s_ring_role != RING_ROLE_SPK ||
        (s_phase != AUDIO_PHASE_IDLE && s_phase != AUDIO_PHASE_PLAY)) {
        return;
    }
    ring_write_commit(&s_ring, n);
}

audio_phase_t audio_io_phase(void)     { return (audio_phase_t)s_phase; }
bool     audio_io_mic_active(void)     { return s_phase == AUDIO_PHASE_REC; }
bool     audio_io_spk_active(void)     { return s_phase == AUDIO_PHASE_PLAY; }
uint32_t audio_io_mic_overflow(void)   { return s_mic_overflow_chunks; }
uint32_t audio_io_spk_underrun(void)   { return s_spk_underrun; }
size_t audio_io_mic_available(void) {
    return !ring_cancelled() && s_ring_role == RING_ROLE_MIC ? ring_available(&s_ring) : 0;
}
size_t audio_io_spk_free(void) {
    return !ring_cancelled() && s_ring_role == RING_ROLE_SPK ? ring_free_space(&s_ring) : 0;
}
