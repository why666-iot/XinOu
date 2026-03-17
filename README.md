待办：
1.增加配网功能
2.ws接口协议（腾讯在线文档）
3.调试

# 心偶 · 嵌入式端文档

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

### 状态机（核心逻辑在 `main/main.cc`）

```
      上电
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

### 启动流程（含规划中的 BLE 配网）

```
上电
 │
 ├─ 读取 NVS 中的 WiFi 凭据          ← [规划中] 当前为硬编码
 │
 ├─ 有凭据 ──► 尝试连接 WiFi（最多重试 5 次）
 │                 │
 │                 ├─ 成功 ──► 进入语音助手主循环（状态机）
 │                 │
 │                 └─ 失败 ──► 自动进入重配网模式（BLE 广播）[规划中]
 │
 └─ 无凭据 ──► 启动 BLE 广播，等待小程序配网  [规划中]
                   │
              收到 SSID + 密码 + 服务器地址
                   │
              写入 NVS → 重启 → 重走流程
```

> **当前状态**：WiFi 凭据硬编码在 `main/main.cc`，每次换网络需重新编译烧录。BLE 配网为规划中功能（v0.2.0）。

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

## 项目结构

```
speech_commands_recognition_with_llm/
│
├── main/                          # ESP32 固件源码（ESP-IDF / C++）
│   ├── main.cc                    # ★ 核心：状态机、语音识别调度、WiFi 连接
│   │
│   ├── audio_manager.cc/.h        # ★ 音频管理器
│   │                              #   - 录音缓冲区（最长 10 秒）
│   │                              #   - 64KB 环形缓冲区：流式接收服务器音频并边收边播
│   │                              #   - addStreamingAudioChunk() / finishStreamingPlayback()
│   │
│   ├── websocket_client.cc/.h     # WebSocket 客户端封装
│   │                              #   - 发送：文本 JSON 事件 + 二进制音频块
│   │                              #   - 自动重连任务
│   │                              #   - 事件回调（接收服务器推送）
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

**规划中（v0.2.0）：**

```
main/
├── ble_provisioning.cc/.h   # BLE GATT 配网服务（NimBLE 栈）
│                            #   - 广播设备名，等待小程序连接
│                            #   - 暴露 2 个可写 Characteristic：SSID / Password
│                            #   - 写入完成后触发 NVS 存储 + 重启
└── nvs_config.cc/.h         # NVS 持久化配置读写
                             #   - 存储：WiFi 凭据、服务器地址
                             #   - 替代当前 main.cc 中的硬编码宏定义
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

烧录前需在 [main/main.cc](main/main.cc) 修改以下硬编码常量：

```cpp
#define WIFI_SSID   "你的WiFi名"
#define WIFI_PASS   "你的WiFi密码"
#define WS_URI      "ws://服务器IP:8888"
```

> **注意**：BLE 配网功能实现后（v0.2.0），以上硬编码将被移除，改为通过微信小程序动态配置。

---

## 版本路线图

| 版本 | 状态 | 主要内容 |
|------|------|----------|
| v0.1.0 | ✅ 当前 | 基础语音助手：唤醒词 + 连续对话 + 本地命令词，WiFi 硬编码，本地服务器 |
| v0.2.0 | 规划中 | BLE 配网：NVS 存储凭据，微信小程序扫码配网，支持换网络重配 |
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
