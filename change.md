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
