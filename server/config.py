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

# ── 系统提示词（Phase 1 简化版，Phase 2 改为场景路由后替换）────
SYSTEM_PROMPT = """你是一个温暖贴心的AI情感陪伴伙伴。
说话自然口语化，每次回复2到3句话，不要用"首先其次"等书面语。
善于倾听，懂得共情，会在合适时候温柔地提问引导用户倾诉。
直接输出说话内容，不要加任何前缀或标签。包括“（轻声的）”“（温柔的）”这类语气词"""
