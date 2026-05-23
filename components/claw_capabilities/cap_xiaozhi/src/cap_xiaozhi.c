/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_xiaozhi.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_xiaozhi";

#define CAP_XIAOZHI_DEFAULT_CHAT_ID "xiaozhi"

typedef struct {
    SemaphoreHandle_t lock;
    bool lifecycle_started;
    bool chat_initialized;
    bool chat_running;
    bool audio_channel_open;
    bool event_handler_registered;
    esp_xiaozhi_chat_handle_t chat_handle;
    esp_mcp_t *mcp_engine;
    bool has_mqtt_config;
    bool has_websocket_config;
    bool has_activation_code;
    bool has_new_version;
    char chat_id[96];
    char serial_number[96];
    char activation_code[64];
    char activation_message[160];
    cap_xiaozhi_audio_output_callback_t audio_output_callback;
    void *audio_output_user_ctx;
} cap_xiaozhi_state_t;

typedef struct {
    bool lifecycle_started;
    bool chat_initialized;
    bool chat_running;
    bool audio_channel_open;
    bool has_mqtt_config;
    bool has_websocket_config;
    bool has_activation_code;
    bool has_new_version;
    char chat_id[96];
    char serial_number[96];
    char activation_code[64];
    char activation_message[160];
} cap_xiaozhi_status_snapshot_t;

static cap_xiaozhi_state_t s_xiaozhi = {
    .chat_id = CAP_XIAOZHI_DEFAULT_CHAT_ID,
};

static esp_err_t cap_xiaozhi_lock(void)
{
    if (!s_xiaozhi.lock) {
        s_xiaozhi.lock = xSemaphoreCreateMutex();
        if (!s_xiaozhi.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    return xSemaphoreTake(s_xiaozhi.lock, pdMS_TO_TICKS(2000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void cap_xiaozhi_unlock(void)
{
    if (s_xiaozhi.lock) {
        xSemaphoreGive(s_xiaozhi.lock);
    }
}

static int64_t cap_xiaozhi_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static const char *cap_xiaozhi_text_role_str(esp_xiaozhi_chat_text_role_t role)
{
    switch (role) {
    case ESP_XIAOZHI_CHAT_TEXT_ROLE_USER:
        return "user";
    case ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT:
        return "assistant";
    default:
        return "unknown";
    }
}

static const char *cap_xiaozhi_tts_state_str(esp_xiaozhi_chat_tts_state_kind_t state)
{
    switch (state) {
    case ESP_XIAOZHI_CHAT_TTS_STATE_START:
        return "start";
    case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:
        return "stop";
    case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
        return "sentence_start";
    default:
        return "unknown";
    }
}

static esp_err_t cap_xiaozhi_render_json(cJSON *root, char *output, size_t output_size)
{
    char *rendered = NULL;

    if (!root || !output || output_size == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_snapshot_status(cap_xiaozhi_status_snapshot_t *snapshot)
{
    esp_err_t err;

    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->lifecycle_started = s_xiaozhi.lifecycle_started;
    snapshot->chat_initialized = s_xiaozhi.chat_initialized;
    snapshot->chat_running = s_xiaozhi.chat_running;
    snapshot->audio_channel_open = s_xiaozhi.audio_channel_open;
    snapshot->has_mqtt_config = s_xiaozhi.has_mqtt_config;
    snapshot->has_websocket_config = s_xiaozhi.has_websocket_config;
    snapshot->has_activation_code = s_xiaozhi.has_activation_code;
    snapshot->has_new_version = s_xiaozhi.has_new_version;
    strlcpy(snapshot->chat_id, s_xiaozhi.chat_id, sizeof(snapshot->chat_id));
    strlcpy(snapshot->serial_number, s_xiaozhi.serial_number, sizeof(snapshot->serial_number));
    strlcpy(snapshot->activation_code, s_xiaozhi.activation_code, sizeof(snapshot->activation_code));
    strlcpy(snapshot->activation_message,
            s_xiaozhi.activation_message,
            sizeof(snapshot->activation_message));

    cap_xiaozhi_unlock();
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_status_json(char *output, size_t output_size)
{
    cap_xiaozhi_status_snapshot_t snapshot;
    cJSON *root = cJSON_CreateObject();
    esp_err_t err;

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_xiaozhi_snapshot_status(&snapshot);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    cJSON_AddStringToObject(root, "chat_id", snapshot.chat_id);
    cJSON_AddBoolToObject(root, "lifecycle_started", snapshot.lifecycle_started);
    cJSON_AddBoolToObject(root, "chat_initialized", snapshot.chat_initialized);
    cJSON_AddBoolToObject(root, "chat_running", snapshot.chat_running);
    cJSON_AddBoolToObject(root, "audio_channel_open", snapshot.audio_channel_open);
    cJSON_AddBoolToObject(root, "has_mqtt_config", snapshot.has_mqtt_config);
    cJSON_AddBoolToObject(root, "has_websocket_config", snapshot.has_websocket_config);
    cJSON_AddBoolToObject(root, "has_activation_code", snapshot.has_activation_code);
    cJSON_AddBoolToObject(root, "has_new_version", snapshot.has_new_version);
    if (snapshot.serial_number[0]) {
        cJSON_AddStringToObject(root, "serial_number", snapshot.serial_number);
    }
    if (snapshot.activation_code[0]) {
        cJSON_AddStringToObject(root, "activation_code", snapshot.activation_code);
    }
    if (snapshot.activation_message[0]) {
        cJSON_AddStringToObject(root, "activation_message", snapshot.activation_message);
    }

    return cap_xiaozhi_render_json(root, output, output_size);
}

static esp_err_t cap_xiaozhi_error_json(char *output, size_t output_size, esp_err_t err)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    return cap_xiaozhi_render_json(root, output, output_size);
}

static esp_err_t cap_xiaozhi_ok_json(char *output,
                                     size_t output_size,
                                     const char *key,
                                     const char *value,
                                     int int_value,
                                     bool use_int)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    if (key && key[0]) {
        if (use_int) {
            cJSON_AddNumberToObject(root, key, int_value);
        } else if (value) {
            cJSON_AddStringToObject(root, key, value);
        }
    }

    return cap_xiaozhi_render_json(root, output, output_size);
}

static char *cap_xiaozhi_build_payload_json(const char *kind,
                                            const char *key,
                                            const char *value,
                                            const char *text)
{
    cJSON *root = cJSON_CreateObject();
    char *json = NULL;

    if (!root) {
        return NULL;
    }

    if (kind) {
        cJSON_AddStringToObject(root, "kind", kind);
    }
    if (key && value) {
        cJSON_AddStringToObject(root, key, value);
    }
    if (text) {
        cJSON_AddStringToObject(root, "text", text);
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t cap_xiaozhi_publish_event(const char *event_type,
                                           const char *content_type,
                                           const char *message_id,
                                           const char *text,
                                           const char *payload_json)
{
    claw_event_t event = {0};
    int64_t now_ms = cap_xiaozhi_now_ms();

    if (!event_type || !event_type[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(event.source_cap, "cap_xiaozhi", sizeof(event.source_cap));
    strlcpy(event.event_type, event_type, sizeof(event.event_type));
    strlcpy(event.source_channel, "xiaozhi", sizeof(event.source_channel));
    strlcpy(event.chat_id, s_xiaozhi.chat_id, sizeof(event.chat_id));
    strlcpy(event.content_type, content_type ? content_type : "json", sizeof(event.content_type));
    if (message_id && message_id[0]) {
        strlcpy(event.message_id, message_id, sizeof(event.message_id));
        strlcpy(event.correlation_id, message_id, sizeof(event.correlation_id));
    }
    event.timestamp_ms = now_ms;
    event.session_policy = CLAW_EVENT_SESSION_POLICY_EPHEMERAL;
    snprintf(event.event_id, sizeof(event.event_id), "xz-%" PRId64 "-%08" PRIx32, now_ms, esp_random());
    event.text = (char *)(text ? text : "");
    event.payload_json = (char *)(payload_json ? payload_json : "{}");
    return claw_event_router_publish(&event);
}

static esp_err_t cap_xiaozhi_set_audio_channel_open(bool open)
{
    esp_err_t err = cap_xiaozhi_lock();

    if (err != ESP_OK) {
        return err;
    }

    s_xiaozhi.audio_channel_open = open;
    cap_xiaozhi_unlock();
    return ESP_OK;
}

static void cap_xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx)
{
    cap_xiaozhi_audio_output_callback_t callback = NULL;
    void *callback_ctx = NULL;
    esp_err_t err;
    (void)ctx;

    if (!data || len <= 0) {
        return;
    }

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return;
    }
    callback = s_xiaozhi.audio_output_callback;
    callback_ctx = s_xiaozhi.audio_output_user_ctx;
    cap_xiaozhi_unlock();

    if (callback) {
        callback(data, (size_t)len, callback_ctx);
    }
}

static void cap_xiaozhi_chat_event_callback(esp_xiaozhi_chat_event_t event,
                                            void *event_data,
                                            void *ctx)
{
    char *payload_json = NULL;
    (void)ctx;

    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        const esp_xiaozhi_chat_text_data_t *text_data = (const esp_xiaozhi_chat_text_data_t *)event_data;
        const char *role = text_data ? cap_xiaozhi_text_role_str(text_data->role) : "unknown";
        const char *text = (text_data && text_data->text) ? text_data->text : "";
        payload_json = cap_xiaozhi_build_payload_json("text", "role", role, text);
        (void)cap_xiaozhi_publish_event("xiaozhi_text", "text", role, text, payload_json);
        free(payload_json);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI: {
        const char *emoji = event_data ? (const char *)event_data : "";
        payload_json = cap_xiaozhi_build_payload_json("emoji", "emoji", emoji, NULL);
        (void)cap_xiaozhi_publish_event("xiaozhi_emoji", "json", "emoji", emoji, payload_json);
        free(payload_json);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        const esp_xiaozhi_chat_tts_state_t *tts = (const esp_xiaozhi_chat_tts_state_t *)event_data;
        const char *state = tts ? cap_xiaozhi_tts_state_str(tts->state) : "unknown";
        const char *text = (tts && tts->text) ? tts->text : "";
        payload_json = cap_xiaozhi_build_payload_json("tts_state", "state", state, text);
        (void)cap_xiaozhi_publish_event("xiaozhi_tts_state", "json", state, text, payload_json);
        free(payload_json);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD: {
        const char *command = event_data ? (const char *)event_data : "";
        payload_json = cap_xiaozhi_build_payload_json("system_cmd", "command", command, NULL);
        (void)cap_xiaozhi_publish_event("xiaozhi_system_cmd", "json", command, NULL, payload_json);
        free(payload_json);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *error = (const esp_xiaozhi_chat_error_info_t *)event_data;
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "kind", "error");
            cJSON_AddNumberToObject(root, "code", error ? error->code : ESP_FAIL);
            cJSON_AddStringToObject(root, "source", (error && error->source) ? error->source : "unknown");
            payload_json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
        }
        (void)cap_xiaozhi_publish_event("xiaozhi_error", "json", "error", NULL, payload_json);
        free(payload_json);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        (void)cap_xiaozhi_publish_event("xiaozhi_speech_started", "json", "speech_started", NULL, NULL);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        (void)cap_xiaozhi_publish_event("xiaozhi_speech_stopped", "json", "speech_stopped", NULL, NULL);
        break;
    default:
        break;
    }
}

static void cap_xiaozhi_esp_event_handler(void *arg,
                                          esp_event_base_t event_base,
                                          int32_t event_id,
                                          void *event_data)
{
    const char *event_name = "unknown";
    char *payload_json = NULL;
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        event_name = "connected";
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        event_name = "disconnected";
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        event_name = "audio_channel_opened";
        (void)cap_xiaozhi_set_audio_channel_open(true);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        event_name = "audio_channel_closed";
        (void)cap_xiaozhi_set_audio_channel_open(false);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_DATA_INCOMING:
        event_name = "audio_data_incoming";
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        event_name = "server_goodbye";
        break;
    default:
        break;
    }

    payload_json = cap_xiaozhi_build_payload_json("connection", "event", event_name, NULL);
    (void)cap_xiaozhi_publish_event("xiaozhi_connection", "json", event_name, NULL, payload_json);
    free(payload_json);
}

static esp_err_t cap_xiaozhi_register_esp_events_locked(void)
{
    esp_err_t err;

    if (s_xiaozhi.event_handler_registered) {
        return ESP_OK;
    }

    err = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS,
                                     ESP_EVENT_ANY_ID,
                                     cap_xiaozhi_esp_event_handler,
                                     NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_xiaozhi.event_handler_registered = true;
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_fetch_info_locked(void)
{
    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t err;

    err = esp_xiaozhi_chat_get_info(&info);
    if (err != ESP_OK) {
        return err;
    }

    s_xiaozhi.has_mqtt_config = info.has_mqtt_config;
    s_xiaozhi.has_websocket_config = info.has_websocket_config;
    s_xiaozhi.has_activation_code = info.has_activation_code;
    s_xiaozhi.has_new_version = info.has_new_version;
    strlcpy(s_xiaozhi.serial_number,
            info.serial_number ? info.serial_number : "",
            sizeof(s_xiaozhi.serial_number));
    strlcpy(s_xiaozhi.activation_code,
            info.activation_code ? info.activation_code : "",
            sizeof(s_xiaozhi.activation_code));
    strlcpy(s_xiaozhi.activation_message,
            info.activation_message ? info.activation_message : "",
            sizeof(s_xiaozhi.activation_message));

    (void)esp_xiaozhi_chat_free_info(&info);
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_create_mcp_engine_locked(void)
{
    if (s_xiaozhi.mcp_engine) {
        return ESP_OK;
    }

    return esp_mcp_create(&s_xiaozhi.mcp_engine);
}

static esp_err_t cap_xiaozhi_init_chat_locked(void)
{
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    esp_err_t err;

    if (s_xiaozhi.chat_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(cap_xiaozhi_register_esp_events_locked(),
                        TAG,
                        "Failed to register Xiaozhi event handler");
    ESP_RETURN_ON_ERROR(cap_xiaozhi_create_mcp_engine_locked(),
                        TAG,
                        "Failed to create Xiaozhi MCP engine");

    err = cap_xiaozhi_fetch_info_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Xiaozhi get_info failed: %s", esp_err_to_name(err));
        return err;
    }

    config.audio_callback = cap_xiaozhi_audio_callback;
    config.event_callback = cap_xiaozhi_chat_event_callback;
    config.has_mqtt_config = s_xiaozhi.has_mqtt_config;
    config.has_websocket_config = s_xiaozhi.has_websocket_config;
    config.mcp_engine = s_xiaozhi.mcp_engine;
    config.owns_mcp_engine = false;

    err = esp_xiaozhi_chat_init(&config, &s_xiaozhi.chat_handle);
    if (err != ESP_OK) {
        return err;
    }

    s_xiaozhi.chat_initialized = true;
    return ESP_OK;
}

static void cap_xiaozhi_apply_start_config_locked(const cap_xiaozhi_start_config_t *config)
{
    if (config && config->chat_id && config->chat_id[0]) {
        strlcpy(s_xiaozhi.chat_id, config->chat_id, sizeof(s_xiaozhi.chat_id));
    } else if (!s_xiaozhi.chat_id[0]) {
        strlcpy(s_xiaozhi.chat_id, CAP_XIAOZHI_DEFAULT_CHAT_ID, sizeof(s_xiaozhi.chat_id));
    }
}

esp_err_t cap_xiaozhi_start_chat(const cap_xiaozhi_start_config_t *config)
{
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    cap_xiaozhi_apply_start_config_locked(config);

    err = cap_xiaozhi_init_chat_locked();
    if (err == ESP_OK && !s_xiaozhi.chat_running) {
        err = esp_xiaozhi_chat_start(s_xiaozhi.chat_handle);
        if (err == ESP_OK) {
            s_xiaozhi.chat_running = true;
        }
    }

    if (err == ESP_OK && config && config->open_audio_channel) {
        cap_xiaozhi_unlock();
        return cap_xiaozhi_open_audio_channel(config);
    }

    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_stop_chat(void)
{
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized) {
        cap_xiaozhi_unlock();
        return ESP_OK;
    }

    err = esp_xiaozhi_chat_stop(s_xiaozhi.chat_handle);
    if (err == ESP_OK) {
        s_xiaozhi.chat_running = false;
        s_xiaozhi.audio_channel_open = false;
    }

    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_open_audio_channel(const cap_xiaozhi_start_config_t *config)
{
    esp_xiaozhi_chat_audio_t audio = {0};
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized || !s_xiaozhi.chat_running) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (config) {
        audio.format = config->audio_format;
        audio.sample_rate = config->sample_rate;
        audio.channels = config->channels;
        audio.frame_duration = config->frame_duration_ms;
    }

    err = esp_xiaozhi_chat_open_audio_channel(s_xiaozhi.chat_handle, &audio, NULL, 0);
    if (err == ESP_OK) {
        s_xiaozhi.audio_channel_open = true;
    }

    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_close_audio_channel(void)
{
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_xiaozhi_chat_close_audio_channel(s_xiaozhi.chat_handle);
    if (err == ESP_OK) {
        s_xiaozhi.audio_channel_open = false;
    }

    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_send_wake_word(const char *wake_word)
{
    esp_err_t err;

    if (!wake_word || !wake_word[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_xiaozhi_chat_send_wake_word(s_xiaozhi.chat_handle, wake_word);
    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_send_start_listening(int mode)
{
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_xiaozhi_chat_send_start_listening(s_xiaozhi.chat_handle, mode);
    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_send_stop_listening(void)
{
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_xiaozhi_chat_send_stop_listening(s_xiaozhi.chat_handle);
    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_abort_speaking(void)
{
    esp_err_t err;

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_xiaozhi_chat_send_abort_speaking(
              s_xiaozhi.chat_handle,
              ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_send_audio_data(const uint8_t *data, size_t len)
{
    esp_err_t err;

    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_initialized || !s_xiaozhi.audio_channel_open) {
        cap_xiaozhi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_xiaozhi_chat_send_audio_data(s_xiaozhi.chat_handle, (const char *)data, len);
    cap_xiaozhi_unlock();
    return err;
}

esp_err_t cap_xiaozhi_set_audio_output_callback(cap_xiaozhi_audio_output_callback_t callback,
                                                void *user_ctx)
{
    esp_err_t err = cap_xiaozhi_lock();

    if (err != ESP_OK) {
        return err;
    }

    s_xiaozhi.audio_output_callback = callback;
    s_xiaozhi.audio_output_user_ctx = user_ctx;
    cap_xiaozhi_unlock();
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_descriptor_init(void)
{
    esp_err_t err = cap_xiaozhi_lock();

    if (err != ESP_OK) {
        return err;
    }

    if (!s_xiaozhi.chat_id[0]) {
        strlcpy(s_xiaozhi.chat_id, CAP_XIAOZHI_DEFAULT_CHAT_ID, sizeof(s_xiaozhi.chat_id));
    }

    cap_xiaozhi_unlock();
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_descriptor_start(void)
{
    esp_err_t err = cap_xiaozhi_lock();

    if (err != ESP_OK) {
        return err;
    }

    s_xiaozhi.lifecycle_started = true;
    cap_xiaozhi_unlock();
    return ESP_OK;
}

static esp_err_t cap_xiaozhi_descriptor_stop(void)
{
    esp_err_t err;

    err = cap_xiaozhi_stop_chat();
    if (err != ESP_OK) {
        return err;
    }

    err = cap_xiaozhi_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (s_xiaozhi.chat_initialized) {
        err = esp_xiaozhi_chat_deinit(s_xiaozhi.chat_handle);
        if (err != ESP_OK) {
            cap_xiaozhi_unlock();
            return err;
        }
        s_xiaozhi.chat_initialized = false;
        s_xiaozhi.chat_handle = 0;
    }
    if (s_xiaozhi.mcp_engine) {
        err = esp_mcp_destroy(s_xiaozhi.mcp_engine);
        if (err != ESP_OK) {
            cap_xiaozhi_unlock();
            return err;
        }
        s_xiaozhi.mcp_engine = NULL;
    }
    s_xiaozhi.lifecycle_started = false;

    cap_xiaozhi_unlock();
    return ESP_OK;
}

static bool cap_xiaozhi_json_bool(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_value;
}

static int cap_xiaozhi_json_int(cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_value;
}

static const char *cap_xiaozhi_json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void cap_xiaozhi_parse_start_config(const char *input_json,
                                           cap_xiaozhi_start_config_t *config,
                                           cJSON **out_root)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");

    memset(config, 0, sizeof(*config));
    if (!root) {
        root = cJSON_CreateObject();
    }

    if (root) {
        config->chat_id = cap_xiaozhi_json_string(root, "chat_id");
        config->open_audio_channel = cap_xiaozhi_json_bool(root, "open_audio_channel", false);
        config->audio_format = cap_xiaozhi_json_string(root, "audio_format");
        config->sample_rate = cap_xiaozhi_json_int(root, "sample_rate", 0);
        config->channels = cap_xiaozhi_json_int(root, "channels", 0);
        config->frame_duration_ms = cap_xiaozhi_json_int(root, "frame_duration_ms", 0);
    }

    *out_root = root;
}

static esp_err_t cap_xiaozhi_start_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    cap_xiaozhi_start_config_t config;
    cJSON *root = NULL;
    esp_err_t err;
    (void)ctx;

    cap_xiaozhi_parse_start_config(input_json, &config, &root);
    err = cap_xiaozhi_start_chat(&config);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_status_json(output, output_size);
}

static esp_err_t cap_xiaozhi_stop_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    esp_err_t err;
    (void)input_json;
    (void)ctx;

    err = cap_xiaozhi_stop_chat();
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_status_json(output, output_size);
}

static esp_err_t cap_xiaozhi_status_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)input_json;
    (void)ctx;

    return cap_xiaozhi_status_json(output, output_size);
}

static esp_err_t cap_xiaozhi_open_audio_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cap_xiaozhi_start_config_t config;
    cJSON *root = NULL;
    esp_err_t err;
    (void)ctx;

    cap_xiaozhi_parse_start_config(input_json, &config, &root);
    err = cap_xiaozhi_open_audio_channel(&config);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_status_json(output, output_size);
}

static esp_err_t cap_xiaozhi_close_audio_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    esp_err_t err;
    (void)input_json;
    (void)ctx;

    err = cap_xiaozhi_close_audio_channel();
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_status_json(output, output_size);
}

static esp_err_t cap_xiaozhi_wake_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    const char *wake_word = root ? cap_xiaozhi_json_string(root, "wake_word") : NULL;
    esp_err_t err;
    (void)ctx;

    if (!wake_word || !wake_word[0]) {
        cJSON_Delete(root);
        (void)cap_xiaozhi_error_json(output, output_size, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_xiaozhi_send_wake_word(wake_word);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_ok_json(output, output_size, "wake_word", wake_word, 0, false);
}

static esp_err_t cap_xiaozhi_start_listening_execute(const char *input_json,
                                                     const claw_cap_call_context_t *ctx,
                                                     char *output,
                                                     size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    int mode = root ? cap_xiaozhi_json_int(root, "mode", ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO) :
               ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO;
    esp_err_t err;
    (void)ctx;

    err = cap_xiaozhi_send_start_listening(mode);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_ok_json(output, output_size, "mode", NULL, mode, true);
}

static esp_err_t cap_xiaozhi_stop_listening_execute(const char *input_json,
                                                    const claw_cap_call_context_t *ctx,
                                                    char *output,
                                                    size_t output_size)
{
    esp_err_t err;
    (void)input_json;
    (void)ctx;

    err = cap_xiaozhi_send_stop_listening();
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_ok_json(output, output_size, NULL, NULL, 0, false);
}

static esp_err_t cap_xiaozhi_abort_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    esp_err_t err;
    (void)input_json;
    (void)ctx;

    err = cap_xiaozhi_abort_speaking();
    if (err != ESP_OK) {
        (void)cap_xiaozhi_error_json(output, output_size, err);
        return err;
    }

    return cap_xiaozhi_ok_json(output, output_size, NULL, NULL, 0, false);
}

static const claw_cap_descriptor_t s_xiaozhi_descriptors[] = {
    {
        .id = "xiaozhi_gateway",
        .name = "xiaozhi_gateway",
        .family = "xiaozhi",
        .description = "Xiaozhi platform event source and lifecycle owner.",
        .kind = CLAW_CAP_KIND_EVENT_SOURCE,
        .cap_flags = CLAW_CAP_FLAG_EMITS_EVENTS | CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = cap_xiaozhi_descriptor_init,
        .start = cap_xiaozhi_descriptor_start,
        .stop = cap_xiaozhi_descriptor_stop,
    },
    {
        .id = "xiaozhi_start",
        .name = "xiaozhi_start",
        .family = "xiaozhi",
        .description = "Fetch Xiaozhi device info and start the Xiaozhi chat transport. Optionally open the audio channel.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},\"open_audio_channel\":{\"type\":\"boolean\"},\"audio_format\":{\"type\":\"string\"},\"sample_rate\":{\"type\":\"integer\"},\"channels\":{\"type\":\"integer\"},\"frame_duration_ms\":{\"type\":\"integer\"}}}",
        .execute = cap_xiaozhi_start_execute,
    },
    {
        .id = "xiaozhi_stop",
        .name = "xiaozhi_stop",
        .family = "xiaozhi",
        .description = "Stop the active Xiaozhi chat transport.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_xiaozhi_stop_execute,
    },
    {
        .id = "xiaozhi_status",
        .name = "xiaozhi_status",
        .family = "xiaozhi",
        .description = "Return Xiaozhi runtime status and activation metadata.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_xiaozhi_status_execute,
    },
    {
        .id = "xiaozhi_open_audio_channel",
        .name = "xiaozhi_open_audio_channel",
        .family = "xiaozhi",
        .description = "Open the Xiaozhi audio channel with optional audio parameters.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"audio_format\":{\"type\":\"string\"},\"sample_rate\":{\"type\":\"integer\"},\"channels\":{\"type\":\"integer\"},\"frame_duration_ms\":{\"type\":\"integer\"}}}",
        .execute = cap_xiaozhi_open_audio_execute,
    },
    {
        .id = "xiaozhi_close_audio_channel",
        .name = "xiaozhi_close_audio_channel",
        .family = "xiaozhi",
        .description = "Close the Xiaozhi audio channel.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_xiaozhi_close_audio_execute,
    },
    {
        .id = "xiaozhi_send_wake_word",
        .name = "xiaozhi_send_wake_word",
        .family = "xiaozhi",
        .description = "Report an offline wake word to the active Xiaozhi session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"wake_word\":{\"type\":\"string\"}},\"required\":[\"wake_word\"]}",
        .execute = cap_xiaozhi_wake_execute,
    },
    {
        .id = "xiaozhi_start_listening",
        .name = "xiaozhi_start_listening",
        .family = "xiaozhi",
        .description = "Send start-listening to the active Xiaozhi session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"integer\"}}}",
        .execute = cap_xiaozhi_start_listening_execute,
    },
    {
        .id = "xiaozhi_stop_listening",
        .name = "xiaozhi_stop_listening",
        .family = "xiaozhi",
        .description = "Send stop-listening to the active Xiaozhi session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_xiaozhi_stop_listening_execute,
    },
    {
        .id = "xiaozhi_abort_speaking",
        .name = "xiaozhi_abort_speaking",
        .family = "xiaozhi",
        .description = "Abort the current Xiaozhi TTS output.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_xiaozhi_abort_execute,
    },
};

static const claw_cap_group_t s_xiaozhi_group = {
    .group_id = "cap_xiaozhi",
    .plugin_name = "Xiaozhi",
    .version = "0.1.0",
    .descriptors = s_xiaozhi_descriptors,
    .descriptor_count = sizeof(s_xiaozhi_descriptors) / sizeof(s_xiaozhi_descriptors[0]),
};

esp_err_t cap_xiaozhi_register_group(void)
{
    if (claw_cap_group_exists(s_xiaozhi_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_xiaozhi_group);
}
