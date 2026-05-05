"""
LLM 模块：OpenAI 兼容接口（支持 DeepSeek / Qwen 等）

用法：
    # 流式对话（场景分类已内嵌到 system prompt，无需单独调用）
    async for token in llm.stream_chat(messages):
        ...

messages 格式：
    [{"role": "system", "content": "..."}, {"role": "user", "content": "..."}]
"""
import os
import sys
from typing import AsyncIterator

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config


async def stream_chat(messages: list[dict]) -> AsyncIterator[str]:
    """流式调用 LLM，逐 token yield 文字

    每次请求创建独立客户端，避免 HTTP 连接复用导致的串流问题。
    """
    from openai import AsyncOpenAI

    client = AsyncOpenAI(
        api_key=config.LLM_API_KEY,
        base_url=config.LLM_BASE_URL,
    )

    try:
        stream = await client.chat.completions.create(
            model=config.LLM_MODEL,
            messages=messages,
            stream=True,
            max_tokens=300,
            temperature=0.85,
        )

        async for chunk in stream:
            if chunk.choices and chunk.choices[0].delta.content:
                yield chunk.choices[0].delta.content
    finally:
        await client.close()
