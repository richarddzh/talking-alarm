#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

typedef struct doubao_asr_session doubao_asr_session_t;

esp_err_t doubao_asr_session_start(doubao_asr_session_t **out);
// One worker owns the session. PCM chunks are limited to 8192 bytes.
esp_err_t doubao_asr_session_send_audio(doubao_asr_session_t *session,
                                        const uint8_t *pcm,
                                        size_t len,
                                        bool is_last);
// Transfers the result to the caller, who must free it. Session close does
// not free a transferred result; receive/transmit scratch never escapes.
esp_err_t doubao_asr_session_finish(doubao_asr_session_t *session,
                                    char **out_text);
void doubao_asr_session_close(doubao_asr_session_t *session);
