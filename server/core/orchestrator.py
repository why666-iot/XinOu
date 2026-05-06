"""
Orchestrator：串联 ASR → LLM（Tool Calling）→ TTS 全链路

数据流：
    PCM bytes
      → ASR.transcribe()                      → 用户文字
      → LLM.stream_chat_with_tools()          → LLM 决策
        ├─ tool_call select_scene(scene_id)    → 获取场景提示词 → 第二次流式调用
        └─ 直接输出文本 token                   → 简单问候/闲聊
      → 句子切分                                → 逐句送 TTS
      → TTS.synthesize_stream()               → PCM 音频流（实时 yield 给 ESP32）

架构说明：
    使用 OpenAI Function Calling（DeepSeek 兼容）让 LLM 自主调用 select_scene
    工具获取场景专属提示词。LLM 根据用户话语自行决定是否需要场景指南：
      - 有情绪/场景话题：调用 select_scene → 获取提示词 → 第二次调用生成回复
      - 简单问候/闲聊：跳过工具直接回复（仅靠 base.txt 人格设定）
"""
import json
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

# ── Tool 定义 ────────────────────────────────────────────────────
SCENE_TOOL = {
    "type": "function",
    "function": {
        "name": "select_scene",
        "description": "根据用户当前的情感状态和话题，选择最匹配的情感场景，获取该场景的专属回复指南。当用户表达了明确的情绪困扰或特定话题时调用。",
        "parameters": {
            "type": "object",
            "properties": {
                "scene_id": {
                    "type": "string",
                    "enum": sorted(prompt_loader.VALID_SCENES),
                    "description": "最匹配的情感场景ID",
                }
            },
            "required": ["scene_id"],
        },
    },
}

# 追加到 system prompt 的工具使用指引
_TOOL_INSTRUCTION = """

## 场景工具
你可以调用 select_scene 工具来获取特定情感场景的回复指南。当用户表达了明确的情绪困扰、工作压力、家庭矛盾等话题时，调用工具获取专属指南再回复，效果更好。如果只是简单的打招呼或闲聊，直接回复即可。"""


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

    # ── Step 2: 拼接 messages（base.txt + 工具指引）──────────
    base_prompt = prompt_loader.get_base_prompt()
    system_prompt = base_prompt + _TOOL_INSTRUCTION
    messages: list[dict] = [{"role": "system", "content": system_prompt}]
    recent = history[-(config.MAX_HISTORY_TURNS * 2) :]
    messages.extend(recent)
    messages.append({"role": "user", "content": user_text})

    # ── Step 3: LLM 流式输出 → Tool Calling → 句子切分 → TTS ──
    reply_parts: list[str] = []
    scene_id: list[str] = [""]  # 用 list 包装，方便闭包内写入

    async def audio_gen() -> AsyncIterator[bytes]:
        sentence_buf = ""

        async def _process_tokens(token_stream) -> AsyncIterator[bytes]:
            """处理 token 流：句子切分 → TTS"""
            nonlocal sentence_buf
            async for token in token_stream:
                reply_parts.append(token)
                sentence_buf += token
                sentence, sentence_buf = _pop_sentence(sentence_buf)
                if sentence:
                    async for chunk in tts.synthesize_stream(sentence):
                        yield chunk

        try:
            # 第一次调用：带 tools
            async for event_type, data in llm.stream_chat_with_tools(
                messages, [SCENE_TOOL]
            ):
                if event_type == "tool_call":
                    # LLM 请求调用 select_scene
                    tool_name = data["name"]
                    tool_args = data["arguments"]
                    selected_scene = tool_args.get("scene_id", "daily")

                    # 校验场景 ID
                    if selected_scene not in prompt_loader.VALID_SCENES:
                        selected_scene = "daily"

                    scene_id[0] = selected_scene
                    print(f"[TOOL] {tool_name} → {selected_scene}")

                    # 获取场景提示词内容
                    scene_content = prompt_loader.get_scene_content(selected_scene)

                    # 构建完整 messages（含 tool_call + tool result）
                    extended_messages = list(messages) + [
                        {
                            "role": "assistant",
                            "content": None,
                            "tool_calls": [
                                {
                                    "id": "call_scene",
                                    "type": "function",
                                    "function": {
                                        "name": tool_name,
                                        "arguments": json.dumps(
                                            tool_args, ensure_ascii=False
                                        ),
                                    },
                                }
                            ],
                        },
                        {
                            "role": "tool",
                            "tool_call_id": "call_scene",
                            "content": scene_content,
                        },
                    ]

                    # 第二次调用：带场景提示词，流式生成回复
                    async for chunk in _process_tokens(
                        llm.stream_chat_continue(extended_messages)
                    ):
                        yield chunk

                elif event_type == "token":
                    # LLM 直接回复（无需工具调用）
                    reply_parts.append(data)
                    sentence_buf += data
                    sentence, sentence_buf = _pop_sentence(sentence_buf)
                    if sentence:
                        async for chunk in tts.synthesize_stream(sentence):
                            yield chunk

        except Exception as e:
            print(f"[!] LLM 调用异常: {e}")
            import traceback

            traceback.print_exc()

        # 尾部残余文本
        tail = sentence_buf.strip()
        if tail:
            async for chunk in tts.synthesize_stream(tail):
                yield chunk

    return user_text, reply_parts, scene_id, audio_gen()
