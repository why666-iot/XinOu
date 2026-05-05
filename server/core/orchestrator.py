"""
Orchestrator：串联 ASR → LLM → TTS 全链路

数据流：
    PCM bytes
      → ASR.transcribe()           → 用户文字
      → LLM.stream_chat()          → token 流（首行含 <scene>场景ID</scene>）
      → 场景标签解析                → 提取场景 ID（仅用于日志，不送 TTS）
      → 句子切分                    → 逐句送 TTS
      → TTS.synthesize_stream()    → PCM 音频流（实时 yield 给 ESP32）

架构说明：
    场景分类已内嵌到 system prompt（unified.txt），LLM 在单次流式调用中：
      第一步：输出 <scene>场景ID</scene>（内联分类，不送 TTS）
      第二步：紧接着输出正常回复（送 TTS）

    对比旧架构（两次 LLM 调用）：省去约 0.3-0.5s 的分类延迟，且效果更好，
    因为 LLM 在同一上下文中完成理解和回复，无缝衔接。
"""
import os
import re
import sys
from typing import AsyncIterator

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config
from modules import asr, llm, tts, prompt_loader

# 句子结束标点（中英文均支持）
_SENT_END_RE = re.compile(r"[。！？!?…]")
_MIN_SENTENCE_LEN = 6  # 少于 6 字不切分，等待更多 token

# <scene>...</scene> 最大收集长度（超过此长度视为 LLM 未按格式输出）
_SCENE_TAG_MAX_LEN = 80


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
) -> tuple[str, list[str], list[str], AsyncIterator[bytes]]:
    """全链路处理入口

    参数：
        pcm_bytes : ESP32 录制的 PCM 音频
        history   : 当前连接的对话历史（[{"role":..,"content":..}, ...]）

    返回：
        user_text   : ASR 识别的用户文字（若为空表示未识别到语音）
        reply_parts : LLM 回复的 token 片段列表（消费完 audio_gen 后可拼接）
        scene_id    : [scene_string]，可变列表，audio_gen 消费后 scene_id[0] 被填充
        audio_gen   : async generator，逐块 yield PCM 音频数据
    """
    # ── Step 1: ASR ──────────────────────────────────────────
    user_text = await asr.transcribe(pcm_bytes)
    if not user_text:
        return "", [], [""], _empty_audio()

    print(f"[ASR] {user_text}")

    # ── Step 2: 拼接 messages（统一 prompt，无需预先选场景）──
    system_prompt = prompt_loader.get_unified_prompt()
    messages: list[dict] = [{"role": "system", "content": system_prompt}]
    recent = history[-(config.MAX_HISTORY_TURNS * 2):]
    messages.extend(recent)
    messages.append({"role": "user", "content": user_text})

    # ── Step 3: LLM 流式输出 → 解析场景标签 → 句子切分 → TTS 流式合成 ──
    reply_parts: list[str] = []
    scene_id: list[str] = [""]  # 用 list 包装，方便闭包内写入

    async def audio_gen() -> AsyncIterator[bytes]:
        scene_buf = ""
        scene_done = False
        sentence_buf = ""

        try:
            async for token in llm.stream_chat(messages):
                if not scene_done:
                    scene_buf += token

                    if "</scene>" in scene_buf:
                        end_idx = scene_buf.index("</scene>") + len("</scene>")
                        m = re.search(r"<scene>(.*?)</scene>", scene_buf[:end_idx])
                        if m:
                            candidate = m.group(1).strip()
                            if candidate in prompt_loader.VALID_SCENES:
                                scene_id[0] = candidate

                        rest = scene_buf[end_idx:].lstrip("\n")
                        scene_done = True
                        scene_buf = ""

                        if rest:
                            reply_parts.append(rest)
                            sentence_buf += rest
                            sentence, sentence_buf = _pop_sentence(sentence_buf)
                            if sentence:
                                async for chunk in tts.synthesize_stream(sentence):
                                    yield chunk
                        continue

                    if len(scene_buf) > _SCENE_TAG_MAX_LEN:
                        scene_done = True
                        reply_parts.append(scene_buf)
                        sentence_buf += scene_buf
                        scene_buf = ""
                        sentence, sentence_buf = _pop_sentence(sentence_buf)
                        if sentence:
                            async for chunk in tts.synthesize_stream(sentence):
                                yield chunk
                    continue

                reply_parts.append(token)
                sentence_buf += token
                sentence, sentence_buf = _pop_sentence(sentence_buf)
                if sentence:
                    async for chunk in tts.synthesize_stream(sentence):
                        yield chunk

        except Exception as e:
            print(f"[!] LLM 调用异常: {e}")
            import traceback
            traceback.print_exc()

        tail = sentence_buf.strip()
        if tail:
            async for chunk in tts.synthesize_stream(tail):
                yield chunk

    return user_text, reply_parts, scene_id, audio_gen()
