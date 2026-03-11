# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 智能语音助手，基于 ESP-IDF 框架，复刻小智 AI 的语音交互功能。系统支持：
- 本地语音唤醒（"你好小智"，WakeNet9）
- 本地命令词识别（MultiNet7，开灯/关灯/拜拜等）
- 连续对话模式：通过 WebSocket 将录音实时传输到外部 LLM 服务器，接收语音回复流式播放
- VAD（语音活动检测）和可选噪音抑制（NSNet）

## Build & Flash Commands

```bash
# 配置项目（必须先配置唤醒词和命令词模型）
idf.py menuconfig
# 需要设置：
# - ESP Speech Recognition → Load Multiple Wake Words → CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS
# - ESP Speech Recognition → 中文命令词识别 → CONFIG_SR_MN_CN_MULTINET7_QUANT

# 编译
idf.py build

# 烧录
idf.py flash

# 查看串口日志
idf.py monitor

# 合并操作
idf.py flash monitor
```

## Configuration Before Building

修改 [main/main.cc](main/main.cc) 中的以下常量（无配置文件，直接硬编码）：

| 常量 | 位置 | 说明 |
|------|------|------|
| `WIFI_SSID` / `WIFI_PASS` | ~行79 | WiFi 凭据 |
| `WS_URI` | ~行82 | WebSocket 服务器地址（格式：`ws://IP:8888`） |
| `LED_GPIO` | ~行75 | LED 控制引脚，默认 GPIO21 |
| `COMMAND_TIMEOUT_MS` | ~行126 | 命令词等待超时，默认 5000ms |

## Architecture

### 系统状态机（`main.cc`）

```
STATE_WAITING_WAKEUP → (检测到"你好小智") → STATE_RECORDING → (VAD检测说话结束) → STATE_WAITING_RESPONSE
                                                     ↑                                          |
                                                     └──────── (AI回复播放完毕，连续对话) ──────┘
```

- **STATE_WAITING_WAKEUP**：持续运行 WakeNet 检测
- **STATE_RECORDING**：同时运行 VAD + 实时 WebSocket 流式传输（说话开始后） + MultiNet 本地命令词检测（连续对话模式下）
- **STATE_WAITING_RESPONSE**：等待 WebSocket 二进制音频流，通过 AudioManager 流式播放；收到 ping 包表示音频流结束

### 模块职责

- **[bsp_board.cc/.h](main/bsp_board.cc)**：硬件抽象层，封装 I2S 驱动。`bsp_board_init()` 初始化 INMP441 麦克风，`bsp_audio_init()` 初始化 MAX98357A 功放，`bsp_play_audio_stream()` 用于流式播放（不停止 I2S）
- **[audio_manager.cc/.h](main/audio_manager.cc)**：录音缓冲区管理（最大10秒）+ 32KB 环形缓冲区流式播放。WebSocket 音频块通过 `addStreamingAudioChunk()` 入队，`finishStreamingPlayback()` 在收到 ping 后触发
- **[wifi_manager.cc/.h](main/wifi_manager.cc)**：STA 模式 WiFi 连接，带重试（默认5次）和事件组同步
- **[websocket_client.cc/.h](main/websocket_client.cc)**：封装 ESP-IDF WebSocket 客户端，支持文本/二进制发送、自动重连任务、事件回调

### 音频数据流

```
INMP441(I2S) → bsp_get_feed_data() → [可选 NSNet 降噪] → processed_audio
  ├── WakeNet.detect()                    (STATE_WAITING_WAKEUP)
  ├── vad_process() + MultiNet.detect()   (STATE_RECORDING)
  └── WebSocketClient.sendBinary()        (STATE_RECORDING, 实时流式传输)

WebSocket 服务器 → DATA_BINARY 事件 → AudioManager.addStreamingAudioChunk()
                 → PING 事件       → AudioManager.finishStreamingPlayback()
```

### 本地命令词 ID

命令词 ID 来自 ESP-SR 框架预定义，使用拼音配置：
- 308: "帮我关灯"（bang wo guan deng）
- 309: "帮我开灯"（bang wo kai deng）
- 314: "拜拜"（bai bai）
- 315: 自定义命令（可替换）

### 依赖组件（`idf_component.yml`）

- `espressif/esp-sr: ^2.1.0`：WakeNet + MultiNet + VAD + NSNet 模型
- `espressif/esp_websocket_client: 1.4.0`：WebSocket 协议支持

### 本地音频文件

[main/mock_voices/](main/mock_voices/) 中的 `.h` 文件是 PCM 格式音频数组（由 `.mp3` 转换），通过 `play_audio_with_stop()` 调用 `AudioManager::playAudio()` 播放。

## Hardware Pin Map

| 外设 | GPIO |
|------|------|
| INMP441 SD（数据）| GPIO6 |
| INMP441 WS | GPIO4 |
| INMP441 SCK | GPIO5 |
| MAX98357A DIN | GPIO7 |
| MAX98357A BCLK | GPIO15 |
| MAX98357A LRC | GPIO16 |
| LED | GPIO21 |
