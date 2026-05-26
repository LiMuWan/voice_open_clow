# EchoEar ESP32-S3

This board profile maps the EchoEar ESP32-S3 hardware into ESP-Claw's ESP Board
Manager layout.

## Source

The GPIO mapping is copied from:

- `D:/workspace/xbell_esp32/main/boards/echoear/config.h`
- `D:/workspace/voice_open_clow/external/SCH_Sche_01_EchoEar_M_V2.0_2026-05-20.pdf`

## Build

```bash
cd application/edge_agent
idf.py gen-bmgr-config -c ./boards -b echoear
idf.py build
```

## Main GPIO Mapping

| Function | GPIO |
| --- | ---: |
| I2C SDA | 2 |
| I2C SCL | 1 |
| I2S MCLK | 42 |
| I2S BCLK | 40 |
| I2S WS | 39 |
| I2S DOUT | 41 |
| I2S DIN | 15 |
| ES8311 PA enable | 4 |
| ES8311 address | 0x18 / YAML 0x30 |
| ES7210 address | 0x40 / YAML 0x80 |
| LCD QSPI PCLK | 18 |
| LCD QSPI CS | 14 |
| LCD QSPI DATA0 | 46 |
| LCD QSPI DATA1 | 13 |
| LCD QSPI DATA2 | 11 |
| LCD QSPI DATA3 | 12 |
| LCD RST | 3 |
| LCD backlight | 44 |
| Touch INT | 10 |
| BOOT button | 0 |
| Green LED | 43 |
| SD MISO/D0 | 17 |
| SD SCK/CLK | 16 |
| SD MOSI/CMD | 38 |
| NFC/Eye UART TX (`ECHOEAR_NFC_UART_EYE_TXD`) | 4 |
| NFC/Eye UART RX (`ECHOEAR_NFC_UART_RXD`) | 5 |
| HRV UART TX | 45 |
| HRV UART RX | 8 |
| GSR ADC | GPIO6 / ADC1_CH5 |

## PCB Variant Note

The source xbell board code detects a later PCB variant at runtime and switches
some pins:

| Signal | Default | Alternate |
| --- | ---: | ---: |
| I2S DIN | 15 | 3 |
| PA enable | 4 | 15 |
| LCD RST | 3 | 47 |
| touch key 2 | NC | 6 |
| legacy `UART1_TX` / `UART1_RX` variables | 6/5 | 5/4 |

ESP Board Manager board YAML is static. This profile uses the default mapping
from `config.h`; add a second board profile if the alternate PCB variant needs
to be built explicitly.

## Scope

This profile configures board-level devices and pin occupation only. The
business sensor protocols from the xbell project, such as HRV frame parsing,
GSR sampling policy, NFC reporting, and eye-display commands, are intentionally
not copied into this ESP-Claw board profile.
