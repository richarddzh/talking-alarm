#pragma once

#include <esp_err.h>
#include <stdbool.h>

typedef void (*doubao_tts_status_cb_t)(void *ctx, const char *msg);
typedef bool (*doubao_tts_cancel_cb_t)(void *ctx);

esp_err_t doubao_tts_play_text(const char *text,
                               doubao_tts_status_cb_t status_cb,
                               void *status_ctx);

// Callback must remain valid until return; only read/latch flags, never block.
// It can also run under the audio admission lock.
// Cancellation is sticky, suppresses PCM/status, and returns INVALID_STATE.
// Network cleanup remains on the worker and may wait for HTTP timeout.
esp_err_t doubao_tts_play_text_cancellable(const char *text,
                                           doubao_tts_status_cb_t status_cb,
                                           void *status_ctx,
                                           doubao_tts_cancel_cb_t cancel_cb,
                                           void *cancel_ctx);
