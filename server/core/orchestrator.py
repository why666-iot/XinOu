"""
Orchestrator：串联 ASR → 场景分类 → LLM → TTS 全链路

数据流：
    PCM bytes
      → ASR.transcribe()           → 用户文字
      → LLM.classify_scene()       → 场景 ID
      → prompt_loader              → 场景化 system_prompt
      → LLM.stream_chat()          → token 流
      → 句子切分                    → 逐句送 TTS
      → TTS.synthesize_stream()    → PCM 音频流（实时 yield 给 ESP32）

延迟优化：LLM 每生成一个完整句子，立即送 TTS，不等全部生成完。
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

# 短语句阈值：低于此字数的输入沿用上次场景，避免 "嗯"/"是啊" 导致场景跳变
_SHORT_TEXT_THRESHOLD = 4


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


async def _classify_with_stickiness(
    user_text: str,
    prev_scene: str,
    classify_context: list[str],
) -> str:
    """带粘滞逻辑的场景分类

    规则：
    1. 短语句（≤4字）且有上次场景 → 直接沿用
    2. 当前在 daily / 无场景 → 新场景需 ≥ SCENE_ENTER_THRESHOLD 才接受
    3. 当前在非 daily 场景 → 切换到另一个非 daily 场景需 ≥ SCENE_SWITCH_THRESHOLD
    4. 置信度不够时，累积上下文重新分类（最多 SCENE_CONTEXT_MAX_TURNS 轮）

    返回：本轮使用的场景 ID
    """
    # ── 短语句：直接沿用 ──
    if len(user_text) <= _SHORT_TEXT_THRESHOLD and prev_scene:
        print(f"[分类] 短语句，沿用上次场景: {prev_scene}")
        return prev_scene

    # ── 累积上下文用于分类 ──
    classify_context.append(user_text)
    # 只保留最近 N 轮
    if len(classify_context) > config.SCENE_CONTEXT_MAX_TURNS:
        classify_context[:] = classify_context[-config.SCENE_CONTEXT_MAX_TURNS:]

    # ── 调用分类器 ──
    # 第一次只用当前文本；如果上下文有多轮，传入完整上下文
    ctx = classify_context if len(classify_context) > 1 else None
    new_scene, confidence = await llm.classify_scene(user_text, context_texts=ctx)

    is_prev_daily = (not prev_scene) or (prev_scene == "daily")
    is_new_daily = (new_scene == "daily")

    # ── 场景决策 ──
    if is_new_daily:
        # 分类器认为是 daily
        if is_prev_daily:
            # daily → daily，正常
            classify_context.clear()
            return "daily"
        else:
            # 非 daily → daily：需要高置信度才允许退出当前场景
            if confidence >= config.SCENE_ENTER_THRESHOLD:
                print(f"[分类] 退出场景 {prev_scene} → daily (置信度 {confidence:.2f} ≥ {config.SCENE_ENTER_THRESHOLD})")
                classify_context.clear()
                return "daily"
            else:
                print(f"[分类] 保持场景 {prev_scene}（daily 置信度 {confidence:.2f} 不足以退出）")
                return prev_scene
    else:
        # 分类器返回非 daily 场景
        if is_prev_daily:
            # daily → 非 daily：需要 SCENE_ENTER_THRESHOLD
            if confidence >= config.SCENE_ENTER_THRESHOLD:
                print(f"[分类] 进入场景 {new_scene} (置信度 {confidence:.2f} ≥ {config.SCENE_ENTER_THRESHOLD})")
                classify_context.clear()
                return new_scene
            else:
                # 置信度不够，保持 daily，但保留上下文供下轮累积
                print(f"[分类] 暂不进入 {new_scene} (置信度 {confidence:.2f} < {config.SCENE_ENTER_THRESHOLD})，累积上下文({len(classify_context)}轮)，保持 daily")
                return "daily"
        elif new_scene == prev_scene:
            # 同场景，直接确认
            classify_context.clear()
            return new_scene
        else:
            # 非 daily → 另一个非 daily：需要 SCENE_SWITCH_THRESHOLD
            if confidence >= config.SCENE_SWITCH_THRESHOLD:
                print(f"[分类] 切换场景 {prev_scene} → {new_scene} (置信度 {confidence:.2f} ≥ {config.SCENE_SWITCH_THRESHOLD})")
                classify_context.clear()
                return new_scene
            else:
                print(f"[分类] 保持场景 {prev_scene}（{new_scene} 置信度 {confidence:.2f} 不足以切换）")
                return prev_scene


async def process(
    pcm_bytes: bytes,
    history: list[dict],
    prev_scene: str = "",
    classify_context: list[str] | None = None,
) -> tuple[str, str, list[str], AsyncIterator[bytes]]:
    """全链路处理入口

    参数：
        pcm_bytes         : ESP32 录制的 PCM 音频
        history           : 当前连接的对话历史（[{"role":..,"content":..}, ...]）
        prev_scene        : 上一轮的场景 ID（短语句时沿用，避免场景跳变）
        classify_context  : 场景分类上下文累积列表（跨轮共享，由调用方维护）

    返回：
        user_text   : ASR 识别的用户文字（若为空表示未识别到语音）
        scene       : 本轮使用的场景 ID（调用方保存，下轮传入 prev_scene）
        reply_parts : LLM 回复的 token 片段列表（消费完 audio_gen 后可拼接）
        audio_gen   : async generator，逐块 yield PCM 音频数据
    """
    if classify_context is None:
        classify_context = []

    # ── Step 1: ASR ──────────────────────────────────────────
    user_text = await asr.transcribe(pcm_bytes)
    if not user_text:
        return "", prev_scene, [], _empty_audio()

    print(f"[ASR] {user_text}")

    # ── Step 2: 场景分类（带粘滞逻辑）──────────────────────────
    scene = await _classify_with_stickiness(user_text, prev_scene, classify_context)

    system_prompt = prompt_loader.get_system_prompt(scene)

    # ── Step 3: 拼接 messages ────────────────────────────────
    messages: list[dict] = [{"role": "system", "content": system_prompt}]
    # 保留最近 N 轮历史，避免 context 过长
    recent = history[-(config.MAX_HISTORY_TURNS * 2) :]
    messages.extend(recent)
    messages.append({"role": "user", "content": user_text})

    # ── Step 4: LLM 流式输出 → 句子切分 → TTS 流式合成 ────────
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

    return user_text, scene, reply_parts, audio_gen()
