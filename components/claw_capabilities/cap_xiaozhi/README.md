# ESP-Claw Xiaozhi Capability

`cap_xiaozhi` adapts Espressif's `esp_xiaozhi` component into the ESP-Claw capability system.

## Scope

This component owns the Xiaozhi protocol lifecycle:

- fetch device/server information with `esp_xiaozhi_chat_get_info`
- initialize and start the Xiaozhi chat transport
- open/close the Xiaozhi audio channel
- report wake word and listening control messages
- forward Xiaozhi text, emoji, TTS, connection, system-command, and error events into `claw_event_router`

It deliberately does not own board-specific microphone capture or speaker playback. Board/application code should bridge audio through:

- `cap_xiaozhi_send_audio_data()` for encoded uplink audio
- `cap_xiaozhi_set_audio_output_callback()` for downlink audio playback

The capability creates a minimal MCP engine for the Xiaozhi client. It keeps
all Xiaozhi-facing callables restricted, so they are usable by system code or
the console but are not exposed as LLM tools by default.

## Enable

Enable `CONFIG_APP_CLAW_CAP_XIAOZHI` in menuconfig or add it to an application sdkconfig defaults file:

```ini
CONFIG_APP_CLAW_CAP_MCP_CLIENT=n
CONFIG_APP_CLAW_CAP_MCP_SERVER=n
CONFIG_APP_CLAW_CAP_XIAOZHI=y
```

The component depends on:

```yaml
espressif/esp_xiaozhi: "^0.1.0"
espressif/mcp-c-sdk: "^1.0.0"
```

`esp_xiaozhi` 0.1.0 currently depends on `mcp-c-sdk` 1.x. ESP-Claw's built-in
MCP client/server capabilities currently depend on `mcp-c-sdk` 2.x, so
`CONFIG_APP_CLAW_CAP_XIAOZHI` is mutually exclusive with
`CONFIG_APP_CLAW_CAP_MCP_CLIENT` and `CONFIG_APP_CLAW_CAP_MCP_SERVER` until the
dependency versions converge.

## Capability Group

The registered group id is `cap_xiaozhi`. It contributes restricted control capabilities:

- `xiaozhi_start`
- `xiaozhi_stop`
- `xiaozhi_status`
- `xiaozhi_open_audio_channel`
- `xiaozhi_close_audio_channel`
- `xiaozhi_send_wake_word`
- `xiaozhi_start_listening`
- `xiaozhi_stop_listening`
- `xiaozhi_abort_speaking`

These capabilities are marked restricted so they are available to system/control surfaces without being exposed to the LLM by default.
