# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

XiaoZhi (小智) is an AI chatbot running on ESP32 microcontrollers. It provides voice interaction with LLM backends (Qwen, DeepSeek, Doubao) via streaming audio over WebSocket/MQTT+UDP protocols. The firmware supports 50+ board configurations with various displays, audio codecs, and connectivity options (WiFi or ML307 4G).

## Build Commands

```bash
# Set target chip (esp32, esp32s3, esp32c3)
idf.py set-target esp32s3

# Build firmware
idf.py build

# Flash to device
idf.py flash

# Monitor serial output
idf.py monitor

# Build and flash
idf.py flash monitor

# Build release package for a specific board
python scripts/release.py [board-directory-name]

# Build all board variants
python scripts/release.py all
```

Board selection is done via `idf.py menuconfig` under "Board Type" or by appending sdkconfig options.

## Architecture

### Core Components

- **Application** (`main/application.cc`): Singleton managing device state machine, audio I/O loop, Opus encoding/decoding, and protocol handling. States: Starting, WifiConfiguring, Idle, Connecting, Listening, Speaking, Upgrading.

- **Board** (`main/boards/common/board.h`): Abstract base class for hardware abstraction. Two main subclasses:
  - `WifiBoard`: WiFi-connected boards
  - `ML307Board`: 4G Cat.1 modem boards

  Each board provides: `GetAudioCodec()`, `GetDisplay()`, `GetBacklight()`, `GetLed()`, and network factory methods.

- **Protocol** (`main/protocols/protocol.h`): Abstract communication layer with implementations:
  - `WebSocketProtocol`: WebSocket-based streaming
  - `MqttProtocol`: MQTT for control + UDP for audio

- **ThingManager** (`main/iot/thing_manager.h`): IoT device registry enabling AI voice control of peripherals (Speaker, Screen, Lamp, Battery). Devices register properties and methods that the LLM can invoke.

### Board Configuration

Each board lives in `main/boards/<board-name>/` with:
- `config.h`: GPIO pins, display params, audio codec settings
- `config.json`: Target chip, flash size, sdkconfig options
- `*_board.cc`: Board class implementation using `DECLARE_BOARD()` macro

### Audio Pipeline

1. I2S input → Opus encoder → Protocol send
2. Protocol receive → Opus decoder → I2S output

Audio codecs supported: ES8311, ES8388, Box codec, and board-specific implementations.

### Display System

- `Display` base class with `LcdDisplay` (SPI/QSPI TFT) and `OledDisplay` (I2C OLED) implementations
- Uses LVGL for UI rendering
- Fonts embedded in `main/assets/`

### Wake Word Detection

Optional ESP-SR integration for offline wake word detection (`CONFIG_USE_WAKE_WORD_DETECT`).

## Key Configuration Options

Configured via `idf.py menuconfig`:
- `CONFIG_BOARD_TYPE_*`: Select board variant
- `CONFIG_CONNECTION_TYPE_WEBSOCKET` / `CONFIG_CONNECTION_TYPE_MQTT_UDP`: Protocol selection
- `CONFIG_USE_WAKE_WORD_DETECT`: Enable offline wake word
- `CONFIG_USE_AUDIO_PROCESSOR`: Enable audio processing (AEC)
- `CONFIG_LANGUAGE_*`: UI language (zh-CN, en-US, ja-JP, zh-TW)

## Code Style

- Google C++ style
- ESP-IDF 5.3+ required
- Chinese comments are common throughout the codebase
