"""
LLM 模块：OpenAI 兼容接口（支持 DeepSeek / Qwen 等）

用法：
    # 场景分类（Phase 2）
    scene = await llm.classify_scene(user_text)

    # 流式对话
    async for token in llm.stream_chat(messages):
        ...

messages 格式：
    [{"role": "system", "content": "..."}, {"role": "user", "content": "..."}]
"""
import json
import os
import re
import sys
from typing import AsyncIterator

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config
from modules import prompt_loader

# 复用同一个 AsyncOpenAI 客户端（共享连接池）
_client = None


def _get_client():
    global _client
    if _client is None:
        from openai import AsyncOpenAI
        _client = AsyncOpenAI(
            api_key=config.LLM_API_KEY,
            base_url=config.LLM_BASE_URL,
        )
    return _client


async def classify_scene(user_text: str, context_texts: list[str] | None = None) -> tuple[str, float]:
    """场景分类：根据用户文本判断话题场景

    复用同一个 DeepSeek API，非流式调用。
    任何异常均降级到 "daily"。

    参数：
        user_text     : 当前用户输入
        context_texts : 可选，累积的最近几轮用户文本（用于置信度不够时增强分类）

    返回：(场景 ID, 置信度) 元组
    """
    # 构造分类输入：有上下文时拼接多轮文本
    if context_texts and len(context_texts) > 1:
        # 把多轮文本拼接，让分类器看到完整对话脉络
        input_text = "\n".join(f"- {t}" for t in context_texts)
    else:
        input_text = user_text

    messages = [
        {"role": "system", "content": prompt_loader.get_classifier_prompt()},
        {"role": "user", "content": input_text},
    ]

    try:
        resp = await _get_client().chat.completions.create(
            model=config.LLM_MODEL,
            messages=messages,
            max_tokens=80,
            temperature=0.1,
        )

        content = resp.choices[0].message.content
        if not content:
            print("[分类] LLM 返回空内容，降级为 daily")
            return "daily", 0.0

        raw = content.strip()

        # 尝试提取 JSON：优先匹配 ```json...```，其次匹配任意 {...}
        match = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", raw, re.DOTALL)
        if match:
            raw = match.group(1)
        elif not raw.startswith("{"):
            # 兜底：从文本中提取第一个 {...}
            match = re.search(r"\{[^{}]*\}", raw)
            if match:
                raw = match.group(0)

        result = json.loads(raw)
        scene = result.get("scene", "daily")
        confidence = float(result.get("confidence", 0))

        if scene not in prompt_loader.VALID_SCENES:
            print(f"[分类] 未知场景 '{scene}'，降级为 daily")
            return "daily", 0.0

        if confidence < config.CLASSIFIER_CONFIDENCE_THRESHOLD:
            print(f"[分类] {scene} (置信度 {confidence:.2f} < {config.CLASSIFIER_CONFIDENCE_THRESHOLD})，降级为 daily")
            return "daily", confidence

        print(f"[分类] {scene} (置信度 {confidence:.2f})")
        return scene, confidence

    except Exception as e:
        print(f"[分类] 异常，降级为 daily: {e}")
        return "daily", 0.0


async def stream_chat(messages: list[dict]) -> AsyncIterator[str]:
    """流式调用 LLM，逐 token yield 文字

    使用 OpenAI Python SDK（兼容 DeepSeek / Qwen / 任意 OpenAI 兼容接口）。
    """
    stream = await _get_client().chat.completions.create(
        model=config.LLM_MODEL,
        messages=messages,
        stream=True,
        max_tokens=300,
        temperature=0.85,
    )

    async for chunk in stream:
        if chunk.choices and chunk.choices[0].delta.content:
            yield chunk.choices[0].delta.content
