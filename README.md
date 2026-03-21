# 心偶 · 嵌入式端文档 v0.2.0

> 本文档面向 AI / 软件方向队友，帮助快速理解嵌入式侧的工作原理、数据流和对接协议。

---

## 硬件平台

**ESP32-S3 N8R8**（8MB Flash + 8MB PSRAM）

| 外设 | 型号 | 作用 |
|------|------|------|
| 麦克风 | INMP441（I2S） | 采集用户语音，16kHz 单声道 |
| 功放+喇叭 | MAX98357A（I2S） | 播放 AI 语音回复 |
| 加速度计 | LIS3DH（I2C） | 检测玩偶晃动/交互动作（预留） |
| LED | 单色 LED | 状态指示（GPIO21） |
| 电源 | TP4056 + TPS63020 | 锂电池充放电 + 升降压稳压 |

---

## 系统架构

### 启动流程

```
上电
 │
 ├─ nvs_config_load()：从 NVS 读取 WiFi 凭据
 │       └─ 若 NVS 为空（首次上电/擦除后）→ 写入编译期默认值（WIFI_SSID_DEFAULT）
 │
 ├─ ble_provisioning_start()：启动 BLE 广播（始终运行，供随时配网）
 │
 ├─ WiFiManager.connect()：尝试连接 WiFi（最多重试 5 次）
 │       ├─ 成功 ──► 连接 WebSocket，进入语音助手主循环
 │       └─ 失败 ──► 打印提示，阻塞等待 BLE 写入新凭据
 │                        │
 │                   BLE 收到 SSID + Password
 │                        │
 │                   写入 NVS → 自动重启 → 重走流程
 │
 └─ 进入语音助手状态机（见下方）
```

### 语音助手状态机

```
      上电/重启
          │
          ▼
┌─────────────────────┐
│  STATE_WAITING_WAKEUP│  ← 持续运行 WakeNet，等待唤醒词"你好小智"
└─────────┬───────────┘
          │ 检测到唤醒词
          ▼
┌─────────────────────┐
│   STATE_RECORDING   │  ← 同时做三件事：
│                     │    1. VAD 检测说话状态
│                     │    2. 实时将音频块流式发送给服务器（WebSocket）
│                     │    3. MultiNet 本地检测命令词
└─────────┬───────────┘
          │ VAD 检测到说话结束（或命令词触发）
          ▼
┌─────────────────────┐
│ STATE_WAITING_RESPONSE │  ← 等待服务器通过 WebSocket 推回音频流
│                     │    收到 PING 包 → 音频流结束，循环回录音
└─────────────────────┘
          │ AI 回复播放完毕（连续对话模式）
          └──────────────────────► STATE_RECORDING（下一轮）
```

### 音频数据流

```
INMP441(I2S)
    │  16kHz, 16bit, 单声道 PCM
    ▼
bsp_get_feed_data()
    │
    ├──► WakeNet.detect()         [STATE_WAITING_WAKEUP]
    │
    ├──► vad_process()            [STATE_RECORDING]
    ├──► MultiNet.detect()        [STATE_RECORDING] → 本地命令词
    └──► WebSocketClient.sendBinary() → 服务器       → AI 对话

服务器 ──► WebSocket DATA_BINARY ──► AudioManager.addStreamingAudioChunk()
服务器 ──► WebSocket PING        ──► AudioManager.finishStreamingPlayback()
                                              │
                                         MAX98357A(I2S) → 喇叭
```

---

## BLE 配网（v0.2.0 新增）

### 概述

设备上电后**始终开启 BLE 广播**，支持随时通过手机写入 WiFi 凭据。凭据写入 NVS 后设备自动重启并连接新 WiFi，无需重新编译烧录。

### BLE 服务规格

| 项目 | 值 |
|------|----|
| 协议栈 | NimBLE（比 Bluedroid 轻约 50-80KB） |
| 广播名 | `XinOu-XXXX`（XXXX = MAC 后 4 位，每台设备唯一） |
| 服务 UUID | `0xFF01` |

| Characteristic | UUID | 权限 | 说明 |
|----------------|------|------|------|
| SSID | `0xFF02` | READ / WRITE | WiFi 名称，最大 32 字节 |
| Password | `0xFF03` | WRITE only | WiFi 密码，最大 64 字节（安全考虑不可读回） |

> 两个 Characteristic 均有 `0x2901 User Description` 描述符，可用 nRF Connect 等工具读取确认字段含义。

### 写入操作流程（必须按顺序）

```
1. 扫描并连接 BLE 设备（名称：XinOu-XXXX）
        │
2. 写入 0xFF02（SSID）← 必须先写
        │
3. 写入 0xFF03（Password）← 触发保存和重启
        │
4. 设备自动重启，连接新 WiFi
```

> **注意**：Password 必须在 SSID 之后写入，否则设备会忽略本次 Password。SSID 写入后不会立即触发任何动作。

### 调试工具

推荐使用 **nRF Connect for Mobile**（Android/iOS 均有）：

1. 打开 nRF Connect → SCANNER → 找到 `XinOu-XXXX`
2. 点 CONNECT → 进入 CLIENT 选项卡
3. 展开 `Unknown Service (0xFF01)`
4. 点 0xFF02 右侧的写入按钮 → 选 UTF-8 → 输入 WiFi 名称 → SEND
5. 点 0xFF03 右侧的写入按钮 → 选 UTF-8 → 输入 WiFi 密码 → SEND
6. 等待约 1 秒，设备自动重启

> 服务和 Characteristic 显示为 "Unknown" 是正常现象——nRF Connect 只识别 Bluetooth SIG 官方 UUID，自定义 UUID（0xFF01 等）均显示 Unknown，不影响读写功能。

### WS_URI（服务器地址）

WS_URI **不通过 BLE 配置**，编译进固件：

```cpp
// main/main.cc 约第 89 行
#define WS_URI_DEFAULT "ws://10.225.67.53:8888"
```

换服务器时修改此宏并重新编译烧录。

---

## 项目结构

```
speech_commands_recognition_with_llm/
│
├── main/                          # ESP32 固件源码（ESP-IDF / C++）
│   ├── main.cc                    # ★ 核心：状态机、语音识别调度、WiFi/BLE 初始化
│   │
│   ├── audio_manager.cc/.h        # ★ 音频管理器
│   │                              #   - 录音缓冲区（最长 15 秒环形）
│   │                              #   - 64KB 环形缓冲区：流式接收服务器音频并边收边播
│   │                              #   - addStreamingAudioChunk() / finishStreamingPlayback()
│   │
│   ├── ble_provisioning.cc/.h     # ★ BLE 配网服务（v0.2.0 新增）
│   │                              #   - NimBLE GATT 服务，暴露 SSID / Password 可写 Characteristic
│   │                              #   - 广播设备名 XinOu-XXXX，写入后触发回调
│   │
│   ├── nvs_config.cc/.h           # NVS 持久化配置（v0.2.0 新增）
│   │                              #   - nvs_config_load()：读取或初始化 WiFi 凭据
│   │                              #   - nvs_config_save_wifi()：BLE 配网后更新凭据
│   │
│   ├── websocket_client.cc/.h     # WebSocket 客户端封装
│   │                              #   - 发送：文本 JSON 事件 + 二进制音频块
│   │                              #   - 自动重连任务，事件回调
│   │
│   ├── bsp_board.cc/.h            # 硬件抽象层（BSP）
│   │                              #   - I2S 初始化（INMP441 麦克风 / MAX98357A 功放）
│   │                              #   - bsp_get_feed_data()：读取麦克风 PCM
│   │                              #   - bsp_play_audio_stream()：流式写入播放 I2S
│   │
│   ├── wifi_manager.cc/.h         # WiFi STA 模式连接（带重试 + 事件组同步）
│   │
│   ├── mock_voices/               # 本地提示音（PCM 数组，由 .mp3 转换而来）
│   │   ├── hi.h                   #   唤醒成功提示音
│   │   ├── ok.h                   #   命令执行确认音
│   │   ├── bye.h                  #   再见音
│   │   └── custom.h               #   自定义音（可替换）
│   │
│   └── idf_component.yml          # 组件依赖声明
│                                  #   espressif/esp-sr ^2.1.0（WakeNet/MultiNet/VAD）
│                                  #   espressif/esp_websocket_client 1.4.0
│
└── server/                        # 服务器端（Python，运行在 PC / 云服务器）
    ├── server.py                  # ★ 核心：WebSocket 服务，桥接 ESP32 ↔ DashScope
    ├── omni_realtime_client.py    # DashScope qwen-omni-turbo-realtime API 封装
    ├── system_prompt.md           # AI 角色设定（可直接编辑定义"心偶"人格）
    └── requirements.txt           # Python 依赖
```

---

## ESP32 与服务器的通信协议

### ESP32 → 服务器

| 内容 | 格式 | 触发时机 |
|------|------|----------|
| `{"event":"recording_started"}` | JSON 文本帧 | 唤醒成功或连续对话回到录音状态时 |
| 音频数据 | WebSocket 二进制帧，16kHz 16bit PCM | STATE_RECORDING 期间，VAD 检测到说话后实时流式发送 |
| `{"event":"recording_ended"}` | JSON 文本帧 | VAD 检测到说话停止 |
| `{"event":"recording_cancelled"}` | JSON 文本帧 | ESP32 本地命令词命中，取消本次 AI 对话 |

### 服务器 → ESP32

| 内容 | 格式 | 含义 |
|------|------|------|
| 音频数据 | WebSocket 二进制帧，16kHz 16bit PCM | AI 语音回复，流式推送 |
| WebSocket PING 帧 | 标准 PING | 音频流发送完毕的信号 |

---

## 本地命令词（MultiNet7，离线识别）

以下命令词在**无需联网**的情况下本地识别，命中后不发起 AI 对话：

| 命令词 | ID | 触发动作 |
|--------|-----|----------|
| 帮我关灯 | 308 | 关闭 LED（GPIO21） |
| 帮我开灯 | 309 | 点亮 LED（GPIO21） |
| 拜拜 | 314 | 播放再见音，返回等待唤醒 |
| 自定义 | 315 | 可替换为其他命令 |

命令词识别**与录音同步进行**（说话过程中实时检测），说话结束前命中即可触发。

---

## 编译与烧录

```bash
# 1. 首次配置（选择唤醒词和命令词模型）
idf.py menuconfig
# ESP Speech Recognition → Load Multiple Wake Words → WN9_NIHAOXIAOZHI_TTS
# ESP Speech Recognition → 中文命令词识别 → MULTINET7_QUANT

# 2. 编译
idf.py build

# 3. 烧录并查看日志
idf.py flash monitor
```

### 配置项

在 [main/main.cc](main/main.cc) 约第 80-90 行修改：

```cpp
// WiFi 默认凭据（首次上电写入 NVS；留空则强制走 BLE 配网）
#define WIFI_SSID_DEFAULT ""
#define WIFI_PASS_DEFAULT ""

// WebSocket 服务器地址（编译进固件，不通过 BLE 配置）
#define WS_URI_DEFAULT "ws://10.225.67.53:8888"
```

### 首次烧录 / 换网络

**方式 A（BLE 配网，推荐）**：保持 `WIFI_SSID_DEFAULT ""` 为空，烧录后用 nRF Connect 写入 WiFi 凭据即可，无需重新编译。

**方式 B（硬编码，调试用）**：在 `WIFI_SSID_DEFAULT` / `WIFI_PASS_DEFAULT` 填入账密，重新编译烧录。注意若 NVS 里已有旧凭据，需先执行：

```bash
idf.py erase-flash   # 清空 NVS，强制用新默认值
idf.py flash monitor
```

---

## 版本路线图

| 版本 | 状态 | 主要内容 |
|------|------|----------|
| v0.1.0 | ✅ 完成 | 基础语音助手：唤醒词 + 连续对话 + 本地命令词，WiFi 硬编码，本地服务器 |
| v0.2.0 | ✅ 当前 | BLE 配网：NVS 存储凭据，nRF Connect / 小程序配网，支持换网络重配 |
| v0.3.0 | 规划中 | 云端服务器部署，小程序完整配网 UI，产品化独立运行 |
| v1.0.0 | 规划中 | 完整产品：扫码→配网→对话，长期记忆，情感陪伴人格 |

---

## 接线图

```text
麦克风(INMP441) → ESP32开发板
-----------------------------
VDD（麦克风）→ 3.3V            // 接电源正极
GND（麦克风）→ GND            // 接电源负极
SD  （麦克风）→ GPIO6         // 数据线
WS  （麦克风）→ GPIO4         // 左右声道选择
SCK （麦克风）→ GPIO5         // 时钟线
L/R → GND

功放(MAX98357A) → ESP32开发板
-----------------------------
VIN（功放）→ 3.3V            // 接电源正极
GND（功放）→ GND             // 接电源负极
DIN（功放）→ GPIO7           // 音频数据输入
BCLK（功放）→ GPIO15         // 位时钟
LRC（功放）→ GPIO16          // 左右声道时钟
SD → 3.3V                   // 接电源正极
Vin → 3.3V                  // 接电源正极
GAIN → 悬空

LED控制
-------
LED长脚 → GPIO21        // 信号线
LED短脚 → GND           // 接地

三轴加速度计（LIS3DH） → ESP32-S3开发板
--------------------------------------
VCC（传感器） → 3.3V（开发板）     // 供电
GND（传感器） → GND（开发板）      // 接地
SDA（传感器） → GPIO8              // I2C数据线
SCL（传感器） → GPIO9              // I2C时钟线
INT1（传感器）→ GPIO18             // 运动/晃动中断信号（可选）
CS   → 悬空                       // SPI模式用，不使用
SDO  → 悬空                       // SPI模式用，不使用
INT2 → 悬空                       // 第二中断口，可不用
ADC1 → 悬空                       // 模拟功能不用
ADC2 → 悬空                       // 模拟功能不用
ADC3 → 悬空                       // 模拟功能不用

电源系统
-----------------------------
TP4056 1A锂电池充电模块
B+ → 电池正极
B- → 电池负极
OUT+ → TPS63020的VIN
OUT- → TPS63020的GND

TPS63020模块
GND //接地
OUT //电源正极
```
