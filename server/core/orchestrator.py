"""
Orchestrator：串联 ASR → LLM → TTS 全链路

数据流：
    PCM bytes
      → ASR.transcribe()        → 用户文字
      → LLM.stream_chat()       → token 流
      → 句子切分                 → 逐句送 TTS
      → TTS.synthesize_stream() → PCM 音频流（实时 yield 给 ESP32）

延迟优化：LLM 每生成一个完整句子，立即送 TTS，不等全部生成完。
"""
import os
import re
import sys
from typing import AsyncIterator

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config
from modules import asr, llm, tts

# 句子结束标点（中英文均支持）
_SENT_END_RE = re.compile(r"[。！？!?…]")
_MIN_SENTENCE_LEN = 6  # 少于 6 字不切分，等待更多 token


def _pop_sentence(buf: str) -> tuple[str, str]:
    """从 token 缓冲区提取第一个完整句子。

    返回 (sentence, remaining)。
    若未找到句子边界，sentence 为空字符串。
    """
    for i, ch in enumerate(buf):
        if _SENT_END_RE.match(ch) and i + 1 >= _MIN_SENTENCE_LEN:
            return buf[: i + 1].strip(), buf[i + 1 :]
    return "", buf


async def _empty_audio() -> AsyncIterator[bytes]:
    """空的音频生成器（ASR 无结果时使用）"""
    if False:
        yield b""


async def process(
    pcm_bytes: bytes,
    history: list[dict],
) -> tuple[str, list[str], AsyncIterator[bytes]]:
    """全链路处理入口

    参数：
        pcm_bytes : ESP32 录制的 PCM 音频
        history   : 当前连接的对话历史（[{"role":..,"content":..}, ...]）

    返回：
        user_text   : ASR 识别的用户文字（若为空表示未识别到语音）
        reply_parts : LLM 回复的 token 片段列表（消费完 audio_gen 后可拼接）
        audio_gen   : async generator，逐块 yield PCM 音频数据
    """
    # ── Step 1: ASR ──────────────────────────────────────────
    user_text = await asr.transcribe(pcm_bytes)
    if not user_text:
        return "", [], _empty_audio()

    print(f"[ASR] {user_text}")

    # ── Step 2: 拼接 messages ────────────────────────────────
    messages: list[dict] = [{"role": "system", "content": config.SYSTEM_PROMPT}]
    # 保留最近 N 轮历史，避免 context 过长
    recent = history[-(config.MAX_HISTORY_TURNS * 2) :]
    messages.extend(recent)
    messages.append({"role": "user", "content": user_text})

    # ── Step 3: LLM 流式输出 → 句子切分 → TTS 流式合成 ────────
    reply_parts: list[str] = []

    async def audio_gen() -> AsyncIterator[bytes]:
        sentence_buf = ""
        async for token in llm.stream_chat(messages):
            sentence_buf += token
            reply_parts.append(token)

            sentence, sentence_buf = _pop_sentence(sentence_buf)
            if sentence:
                print(f"[LLM→TTS] {sentence}")
                async for chunk in tts.synthesize_stream(sentence):
                    yield chunk

        # 处理末尾残留的文字（最后一句可能没有标点）
        tail = sentence_buf.strip()
        if tail:
            print(f"[LLM→TTS] {tail}")
            async for chunk in tts.synthesize_stream(tail):
                yield chunk

    return user_text, reply_parts, audio_gen()
