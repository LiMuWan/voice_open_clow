/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cap_xiaozhi_audio_output_callback_t)(const uint8_t *data,
                                                    size_t len,
                                                    void *user_ctx);

typedef struct {
    const char *chat_id;
    bool open_audio_channel;
    const char *audio_format;
    int sample_rate;
    int channels;
    int frame_duration_ms;
} cap_xiaozhi_start_config_t;

esp_err_t cap_xiaozhi_register_group(void);
esp_err_t cap_xiaozhi_start_chat(const cap_xiaozhi_start_config_t *config);
esp_err_t cap_xiaozhi_stop_chat(void);
esp_err_t cap_xiaozhi_open_audio_channel(const cap_xiaozhi_start_config_t *config);
esp_err_t cap_xiaozhi_close_audio_channel(void);
esp_err_t cap_xiaozhi_send_wake_word(const char *wake_word);
esp_err_t cap_xiaozhi_send_start_listening(int mode);
esp_err_t cap_xiaozhi_send_stop_listening(void);
esp_err_t cap_xiaozhi_abort_speaking(void);
esp_err_t cap_xiaozhi_send_audio_data(const uint8_t *data, size_t len);
esp_err_t cap_xiaozhi_set_audio_output_callback(cap_xiaozhi_audio_output_callback_t callback,
                                                void *user_ctx);

#ifdef __cplusplus
}
#endif
