# 变更记录

---

## 2026-03-31

### 修复：对话首字/前几帧丢失问题

**文件：** `main/main.cc`

**问题描述：**
每次说话时，服务器收到的音频总是缺少开头的一两个字。

**根本原因：**
系统使用 VAD（语音活动检测）来判断用户是否开始说话，只有 VAD 确认有语音后才开启实时流式传输（`is_realtime_streaming = true`）。但 VAD 本身有几帧的检测延迟（约 30~90ms），在这段时间内麦克风已经录入了音频并存入了录音缓冲区，却没有被发送给服务器，导致首字丢失。

**修改内容：**
在 `is_realtime_streaming` 从 `false` 变为 `true` 的瞬间（VAD 首次检测到语音时），增加了一段"预缓冲补发"逻辑：
- 从 `AudioManager` 的录音缓冲区中导出已积累的全部音频数据
- 分块（每块 4KB）通过 WebSocket 发送给服务器
- 之后再继续正常的实时流式传输

这样服务器就能收到从用户真正开口那一刻起的完整音频。

**修改位置：** `main/main.cc` — `STATE_RECORDING` 处理块，VAD 检测到 `VAD_SPEECH` 后的 `if (!is_realtime_streaming)` 分支内。

---

## 2026-04-01

### 更新：TTS 音色设置

**文件：** `server/config.py`

**修改内容：**
将 TTS 音色从 `longxiaochun_v2` 改为 `siyue`，后又改为 `Qianyue`（用户手动调整）。

---

### 修复：TTS 模型与音色不匹配导致 error 418

**文件：** `server/config.py`

**问题描述：**
TTS 报错 `InvalidParameter / Engine return error code: 418`，请求完全失败。

**根本原因：**
`TTS_VOICE = "Qianyue"` 是千问TTS（qwen-tts）系列的音色名，但 `TTS_MODEL` 仍是 `cosyvoice-v2`。
CosyVoice 不认识 `Qianyue` 这个名字，因此返回参数错误。

**修改内容：**
- `TTS_MODEL`: `cosyvoice-v2` → `qwen-tts`
- `TTS_VOICE`: `Qianyue` → `Cherry`（Cherry 是 qwen-tts 中"芊悦"音色的正式 API 标识符）

---

### 回滚：TTS 模型改回 cosyvoice-v2

**文件：** `server/config.py`, `server/modules/tts.py`

**原因：**
`qwen3-tts-instruct-flash-realtime` 不支持自定义音色（声音克隆），而项目后期计划使用 CosyVoice 的音色复刻功能，因此改回 `cosyvoice-v2`。

**修改内容：**
- `TTS_MODEL`: `qwen3-tts-instruct-flash-realtime` → `cosyvoice-v2`
- `TTS_VOICE`: `""` → `longxiaochun_v2`（默认女声，后续可替换为自定义 voice_id）
- `tts.py`：还原为直接传 `voice` 参数（cosyvoice-v2 必须指定音色）

---

### 更新：ASR 模型名称注释

**文件：** `server/config.py`

**修改内容：**
将 `ASR_MODEL` 的注释从"Paraformer 流式识别"更正为"FunASR 实时识别（新版，替代 paraformer-realtime-v2）"，与当前实际使用的模型名 `fun-asr-realtime` 保持一致。

---

## 2026-04-13

### 修复：音频播放卡顿 + 后续轮次录音丢帧问题

**影响文件：**
- `main/audio_manager.h`
- `main/audio_manager.cc`
- `main/audio_playback_task.cc`（新增）
- `main/CMakeLists.txt`

**问题描述：**
1. **音频播放卡顿**：AI 回复时，音频会卡在某个音节上高速重复播放
2. **后续轮次录音丢帧**：第 1-2 轮对话正常，但从第 3 轮开始，录音只能录到前两个字，中间部分丢失（例如"我现在十分的难过"变成"我在分的难过"）
3. **WebSocket 频繁断线重连**：播放音频时 WebSocket 连接不稳定

**根本原因分析：**

#### 1. 为什么 `bsp_play_audio_stream` 会阻塞 100ms？

`bsp_play_audio_stream` 内部调用 `i2s_channel_write(tx_handle, data, 3200, &written, portMAX_DELAY)`：
- **`portMAX_DELAY`**：无限等待，直到 I2S TX DMA 缓冲区有空间
- 每次传入 **3200 字节**（`STREAMING_CHUNK_SIZE`）
- 采样率 16kHz，单声道，16bit → 每秒 32000 字节
- **3200 字节 = 100ms 音频播放时间**

I2S DMA 工作机制：
- DMA 有硬件缓冲区（几 KB）
- `i2s_channel_write` 把数据拷贝到 DMA 缓冲区
- 如果 DMA 缓冲区满，函数**阻塞等待** DMA 把数据发送到 MAX98357A 功放腾出空间
- 发送 3200 字节需要 100ms 物理时间（音频播放速度固定）

#### 2. 阻塞期间为何 WebSocket 心跳包处理不了？

ESP-IDF 的 WebSocket 客户端运行在独立的 FreeRTOS 任务（`ws_task`），负责：
- 接收 WebSocket 数据（触发 `DATA_BINARY` 回调）
- 发送数据
- **处理 TCP keepalive 和 WebSocket ping/pong 心跳包**

原来的代码在 WebSocket 的 `DATA_BINARY` 事件回调里直接调用：
```cpp
audio_manager.addStreamingAudioChunk(data, len);  // 内部调用 bsp_play_audio_stream，阻塞 100ms
```

**问题链路**：
1. 服务器发来音频数据（3200 字节）
2. WebSocket 任务收到数据，触发 `DATA_BINARY` 回调
3. 回调里调用 `addStreamingAudioChunk` → `bsp_play_audio_stream` → **阻塞 100ms**
4. 这 100ms 期间，**WebSocket 任务被卡住**，无法：
   - 读取 TCP socket（服务器心跳包堆积在内核缓冲区）
   - 发送心跳包响应
   - 处理其他 WebSocket 协议帧
5. 服务器等不到心跳包响应 → 认为连接死了 → **主动断开连接**
6. ESP32 检测到断线 → 重连

#### 3. I2S RX 为何会溢出？

ESP32 有两个独立的 I2S 通道：
- **I2S_NUM_0 (RX)**：连接 INMP441 麦克风，持续录音
- **I2S_NUM_1 (TX)**：连接 MAX98357A 功放，播放音频

I2S RX 工作方式：
1. INMP441 以固定速率（16kHz）产生音频数据
2. I2S RX DMA 把数据搬到环形缓冲区（通常 4-8KB）
3. 代码需要**定期调用 `bsp_get_feed_data()`** 从 DMA 缓冲区读走数据
4. 如果不及时读，DMA 缓冲区满了 → **新数据覆盖旧数据** → 丢帧

主循环（`main.cc`）里：
```cpp
while (state == STATE_RECORDING) {
    bsp_get_feed_data(...);  // 从 I2S RX DMA 读取麦克风数据
    vad_process(...);
    // ...
}
```

但是，当 WebSocket 任务在播放音频时：
- WebSocket 任务调用 `bsp_play_audio_stream` → 阻塞 100ms
- FreeRTOS 调度器可能让 WebSocket 任务继续占用 CPU（等待 I2S TX DMA，优先级较高）
- **主循环拿不到 CPU 时间片** → `bsp_get_feed_data()` 没法及时调用
- I2S RX DMA 缓冲区（假设 8KB）在 **8192 / 32000 = 256ms** 后溢出
- 连续播放多个 100ms 音频块 → **累计阻塞时间超过 256ms** → 溢出

#### 4. 溢出为什么导致录音中间丢帧？

DMA 环形缓冲区溢出时，**新数据覆盖旧数据**（不是丢弃新数据）。

假设说"我现在十分的难过"：
1. 前 2 秒："我现在十" → 正常录入
2. 第 2-3 秒：AI 开始回复，WebSocket 任务播放音频 → 主循环被饿死 → DMA 溢出
3. 第 3 秒："分的难过" → **这部分音频在 DMA 缓冲区里被覆盖了**
4. 第 3.5 秒：播放结束，主循环恢复 → 调用 `bsp_get_feed_data()` → 读到的是**被覆盖后的垃圾数据**
5. 最终录音："我现在十[噪音/静音]"

这就是为什么录音变成"我在分的难过"（中间丢了"现在十"）。

**修改方案：生产者-消费者解耦**

**改动前架构**：
```
WebSocket 任务（生产者 + 消费者）
  ├─ 接收音频数据（生产）
  └─ 调用 bsp_play_audio_stream 播放（消费，阻塞 100ms）
```

**改动后架构**：
```
WebSocket 任务（生产者）
  ├─ 接收音频数据
  └─ malloc + xQueueSend（非阻塞，<1ms）

FreeRTOS 队列（64 槽缓冲区）

独立播放任务（消费者）
  ├─ xQueueReceive（等待数据）
  └─ 调用 bsp_play_audio_stream 播放（阻塞 100ms，但不影响其他任务）
```

**具体修改内容：**

#### `audio_manager.h`
- 新增私有成员：
  ```cpp
  TaskHandle_t  playback_task_handle;
  QueueHandle_t audio_queue;
  volatile bool playback_task_running;
  volatile bool streaming_finished;
  static const size_t AUDIO_QUEUE_SIZE = 64;
  static void playbackTaskFunc(void* param);
  ```
- 新增 `#include "freertos/queue.h"`

#### `audio_manager.cc`

| 函数 | 改动 |
|---|---|
| `init()` | 不再分配 64KB 流式缓冲区，改为创建 64 槽 FreeRTOS 队列（每槽存 `uint8_t*` 指针） |
| `deinit()` | 新增：等待播放任务退出、清空队列、删除队列 |
| `startStreamingPlayback()` | 不再 memset 缓冲区，改为清空队列残留数据，然后用 `xTaskCreate` 启动独立播放任务（优先级 `configMAX_PRIORITIES - 2`，栈 4KB） |
| `addStreamingAudioChunk()` | **关键改动**：删除整个复杂的环形缓冲区 + 直接播放逻辑，换成：<br>1. `malloc(sizeof(size_t) + size)` 分配内存块<br>2. 前 `sizeof(size_t)` 字节存数据长度，后跟音频数据<br>3. `xQueueSend(audio_queue, &block, 0)` 非阻塞投递<br>4. 如果队列满，立即返回 `false`（丢弃这一帧），**零阻塞** |
| `finishStreamingPlayback()` | 不再直接播放尾巴数据，改为：<br>1. 设置 `streaming_finished = true`<br>2. 等待播放任务自然退出（最多 10 秒）<br>3. 超时则强制终止任务并调用 `bsp_audio_stop()` |

#### `audio_playback_task.cc`（新文件）

实现 `AudioManager::playbackTaskFunc`，运行在独立 FreeRTOS 任务里：
1. 循环从队列取音频块：`xQueueReceive(audio_queue, &block, pdMS_TO_TICKS(100))`
2. 从块的前 `sizeof(size_t)` 字节读取数据长度
3. 调用 `bsp_play_audio_stream(data, size)` 播放（阻塞 100ms，但不影响其他任务）
4. 释放内存：`free(block)`
5. 超时时检查：如果 `streaming_finished==true` 且队列为空 → 退出循环
6. 调用 `bsp_audio_stop()` 停止 I2S TX
7. 设置 `playback_task_running = false`，然后 `vTaskDelete(NULL)`

#### `CMakeLists.txt`
- 在 `SRCS` 列表中新增 `audio_playback_task.cc`

**技术细节：**

| 改动点 | 原理 |
|---|---|
| `xQueueSend(queue, &block, 0)` | 第三个参数 `0` → **非阻塞**，队列满时立即返回失败（丢弃这一帧），不会卡住 WebSocket 任务 |
| 独立播放任务 | 运行在自己的 FreeRTOS 任务里，阻塞时只影响自己，**不影响 WebSocket 任务和主循环** |
| 队列大小 64 槽 | 每槽存一个 `uint8_t*` 指针（4 字节），总开销 256 字节。实际音频数据在堆上（每块 3200 字节），64 块 = 204KB，足够缓冲 6.4 秒音频 |
| 内存管理 | 生产者（WebSocket 任务）`malloc` 分配，消费者（播放任务）`free` 释放，避免数据拷贝 |

**效果：**
- WebSocket 回调立即返回（<1ms），心跳包正常处理 → **不再断线**
- 主循环不再被饿死，`bsp_get_feed_data()` 及时调用 → I2S RX DMA 不溢出 → **录音不再丢帧**
- 播放任务独立运行，阻塞不影响其他任务 → **音频播放流畅，不卡顿**

**未修改文件：**
- `main/main.cc`：实时流式发送逻辑保持原样，低延迟不受影响

---

## 2026-05-04

### 优化：流式播放缓冲区改为 PSRAM 预分配环形缓冲区

**影响文件：**
- `main/audio_manager.h`
- `main/audio_manager.cc`
- `main/audio_playback_task.cc`

**问题描述：**

实测日志出现 `音频队列已满，丢弃 3344 字节` 警告，开始播放约 2 秒后队列即已满载：

```
I (81533) AudioManager: 开始流式音频播放
W (83503) AudioManager: 音频队列已满，丢弃 3344 字节
W (83503) AudioManager: 音频队列已满，丢弃 1440 字节
W (83563) AudioManager: 音频队列已满，丢弃 2880 字节
```

**根本原因：**

在局域网环境下，服务端发送 TTS 音频的速度远快于实时播放速度（网络传输 < 1ms，而播放每块需要 100ms 物理时间），导致 64 槽队列迅速堆满。同时，每块音频都经历 `malloc/free`，多轮对话后堆碎片化加剧。

**修改方案：预分配环形缓冲区**

用一块从 PSRAM 预分配的大缓冲区替换 64 槽队列 + per-chunk malloc，彻底消除溢出和碎片化问题。

| 对比项 | 旧方案（队列 + malloc） | 新方案（环形缓冲区） |
|---|---|---|
| 缓冲区大小 | 64 × ~3200 B ≈ 200KB / 6.4s | 320KB / 10s（PSRAM） |
| 内存管理 | 每块 malloc/free，多轮后碎片化 | 一次性预分配，零碎片 |
| 线程安全 | FreeRTOS 队列内置 | 互斥锁（xSemaphoreCreateMutex） |
| 满时行为 | 丢弃新块，打印警告 | 同上，但实际发生概率极低 |

**具体修改内容：**

#### `audio_manager.h`
- 移除 `QueueHandle_t audio_queue`、`AUDIO_QUEUE_SIZE`
- 移除 `#include "freertos/queue.h"`，替换为 `#include "freertos/semphr.h"`
- 新增环形缓冲区字段：
  ```cpp
  uint8_t* ring_buf;               // 预分配缓冲区（PSRAM）
  size_t ring_buf_total;           // 总大小（320KB）
  size_t ring_write_pos;           // 写入位置
  size_t ring_read_pos;            // 读取位置
  volatile size_t ring_data_len;   // 当前有效字节数
  SemaphoreHandle_t ring_mutex;    // 读写互斥锁
  static const size_t RING_BUF_SIZE  = 320 * 1024;
  static const size_t PLAYBACK_CHUNK = 3200;
  ```

#### `audio_manager.cc`
- 新增 `#include "esp_heap_caps.h"`
- `init()`：优先 `heap_caps_malloc(RING_BUF_SIZE, MALLOC_CAP_SPIRAM)`，PSRAM 不足自动降级内部 RAM；创建互斥锁
- `deinit()`：删除互斥锁，释放环形缓冲区
- `startStreamingPlayback()`：持锁重置 `ring_write_pos / ring_read_pos / ring_data_len`
- `addStreamingAudioChunk()`：持锁写入，支持绕回（两段 memcpy），空间不足时丢弃并打印警告
- `finishStreamingPlayback()`：超时等待上限从 10s 提升到 30s，留足 10s 音频播放余量

#### `audio_playback_task.cc`
- 每轮持锁从环形缓冲区读取最多 `PLAYBACK_CHUNK` 字节到**栈上临时缓冲区**，释放锁后再调用 `bsp_play_audio_stream()`
- 缓冲区空但未完成 → `vTaskDelay(5ms)` 让出 CPU
- 缓冲区空且 `streaming_finished` → 退出

**效果：**
- 消除 `音频队列已满` 警告，10 秒以内的 TTS 音频不再有任何丢弃
- 消除多轮对话后的堆碎片化
- 锁持有时间极短（仅 memcpy 期间），对接收侧无影响

---

### 优化：LLM 双阶调用改为单次流式调用（内联场景分类）

**影响文件：**
- `server/prompts/unified.txt`（新建）
- `server/modules/prompt_loader.py`
- `server/modules/llm.py`
- `server/core/orchestrator.py`
- `server/main.py`

**问题描述：**

旧架构每轮对话调用两次 LLM：
1. 非流式调用分类器（~0.3-0.5s 延迟）→ 获取场景 ID
2. 再流式调用主模型 → 生成回复

分类调用本身耗时、结果质量有限（独立判断不如结合上下文），且代码复杂（粘滞逻辑、置信度阈值、上下文累积等 ~130 行）。

**修改方案：单次流式调用 + 提示词内联场景分类**

将场景选择逻辑内嵌到 system prompt，要求 LLM 在一次流式调用中先输出场景标签再输出回复，流式解析器负责过滤标签。

```
LLM 输出示例：
<scene>work/pressure</scene>
嗯，今天加班又很晚啊？听起来最近真的很累。你是一直这么忙还是最近突然压力大了？
```

**具体修改内容：**

#### `server/prompts/unified.txt`（新建）
合并 `base.txt` + 9 个场景提示词 + 两阶段格式指令，要求 LLM 第一行输出 `<scene>场景ID</scene>`，后续输出正常回复。

#### `server/modules/llm.py`
删除 `classify_scene()` 函数及相关导入（`json`、`re`、`prompt_loader`）。

#### `server/modules/prompt_loader.py`
新增 `get_unified_prompt()` 函数，加载并返回 `unified.txt` 内容。

#### `server/core/orchestrator.py`
完全重写：
- 删除 `_classify_with_stickiness()`、粘滞常量、`classify_context` 参数
- `process()` 签名简化为 `(pcm_bytes, history)`，返回 `(user_text, reply_parts, audio_gen)`
- `audio_gen()` 内部新增 `<scene>` 标签解析状态机：
  - 累积 token 直到遇到 `</scene>` → 提取场景 ID 打印日志，不送 TTS
  - 超过 80 字未找到 `</scene>` → 兜底，直接当正文处理
  - 之后所有 token 走正常句子切分 → TTS

#### `server/main.py`
- 删除 `prev_scene`、`classify_context` 变量
- `orchestrator.process()` 调用更新为新签名

**效果：**
- 每轮对话减少一次 LLM API 调用，首字延迟降低约 0.3-0.5s
- 场景判断结合完整对话上下文，准确率更高
- 代码量减少约 130 行（粘滞逻辑全部删除）

---

### 优化：addRecordingData 改用 memcpy

**影响文件：** `main/audio_manager.cc`

**修改内容：**
将 `addRecordingData()` 中逐样本循环写入改为分段 `memcpy`：
- 数据不跨越缓冲区末尾时：单次 memcpy
- 数据跨越末尾时：两段 memcpy（尾部 + 头部）

逻辑等价，代码更规范，减少不必要的函数调用开销。
