"""
后端配置
使用前设置以下环境变量：
    set DASHSCOPE_API_KEY=sk-xxx    # 阿里云 DashScope（ASR + TTS）
    set LLM_API_KEY=sk-xxx          # LLM 服务密钥
    set LLM_BASE_URL=https://...    # LLM 接口地址（可选，默认 DeepSeek）
    set LLM_MODEL=deepseek-chat     # 模型名（可选）
"""
import os

# ── API Keys ─────────────────────────────────────────────────
DASHSCOPE_API_KEY = os.environ.get("DASHSCOPE_API_KEY", "")
LLM_API_KEY       = os.environ.get("LLM_API_KEY", "")

# ── LLM 配置 ─────────────────────────────────────────────────
# DeepSeek V3：base_url=https://api.deepseek.com/v1  model=deepseek-chat
# Qwen（DashScope 兼容层）：base_url=https://dashscope.aliyuncs.com/compatible-mode/v1  model=qwen-plus
LLM_BASE_URL = os.environ.get("LLM_BASE_URL", "https://api.deepseek.com/v1")
LLM_MODEL    = os.environ.get("LLM_MODEL", "deepseek-chat")

# ── 服务器 ────────────────────────────────────────────────────
WS_HOST = "0.0.0.0"
WS_PORT = 8888

# ── 音频参数 ──────────────────────────────────────────────────
SAMPLE_RATE = 16000  # ESP32 录音采样率（Hz）

# ── 模型 ──────────────────────────────────────────────────────
ASR_MODEL = "fun-asr-realtime"        # 阿里云 FunASR 实时识别（新版，替代 paraformer-realtime-v2）
TTS_MODEL = "cosyvoice-v2"            # 阿里云 CosyVoice v2 语音合成（支持自定义音色）
TTS_VOICE = "longxiaochun_v2"         # 音色：龙小淳（自然女声）；自定义音色填入 voice_id

# ── 对话设置 ──────────────────────────────────────────────────
MAX_HISTORY_TURNS = 10  # 保留最近 N 轮对话（控制 token 消耗）

# ── 场景分类（Phase 2）──────────────────────────────────────────
CLASSIFIER_CONFIDENCE_THRESHOLD = 0.6   # 基础阈值：低于此值直接降级为 daily
SCENE_ENTER_THRESHOLD           = 0.85  # 入场阈值：从 daily 进入特定场景需要的最低置信度
SCENE_SWITCH_THRESHOLD          = 0.85  # 切换阈值：从一个非 daily 场景切换到另一个非 daily 场景的最低置信度
SCENE_CONTEXT_MAX_TURNS         = 3     # 上下文累积：置信度不够时，最多累积最近 N 轮用户文本喂给分类器
