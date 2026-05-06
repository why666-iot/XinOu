"""
LLM 模块：OpenAI 兼容接口（支持 DeepSeek / Qwen 等）

用法：
    # 纯文本流式对话
    async for token in llm.stream_chat(messages):
        ...

    # 带 tool calling 的流式对话
    async for event_type, data in llm.stream_chat_with_tools(messages, tools):
        if event_type == "tool_call":
            # data = {"name": "...", "arguments": "..."}
        elif event_type == "token":
            # data = "文字"

    # 工具调用后继续流式生成
    async for token in llm.stream_chat_continue(messages):
        ...

messages 格式：
    [{"role": "system", "content": "..."}, {"role": "user", "content": "..."}]
"""
import json
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


async def stream_chat_with_tools(
    messages: list[dict], tools: list[dict]
) -> AsyncIterator[tuple[str, any]]:
    """流式调用 LLM，支持 function calling

    yields: (type, data)
        ("tool_call", {"name": ..., "arguments": ...})  — 工具调用请求
        ("token", "文字")                                — 文本 token
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
            tools=tools,
            stream=True,
            max_tokens=300,
            temperature=0.85,
        )

        # 用于收集 tool_call 的增量数据
        tool_call_name = ""
        tool_call_args = ""
        has_tool_call = False

        async for chunk in stream:
            if not chunk.choices:
                continue

            delta = chunk.choices[0].delta
            finish_reason = chunk.choices[0].finish_reason

            # 收集 tool_call 增量
            if delta.tool_calls:
                has_tool_call = True
                tc = delta.tool_calls[0]
                if tc.function.name:
                    tool_call_name += tc.function.name
                if tc.function.arguments:
                    tool_call_args += tc.function.arguments

            # 普通文本 token
            if delta.content:
                yield ("token", delta.content)

            # 流结束：tool_calls 完成
            if finish_reason == "tool_calls" and has_tool_call:
                try:
                    args = json.loads(tool_call_args)
                except json.JSONDecodeError:
                    args = {"scene_id": "daily"}
                yield ("tool_call", {"name": tool_call_name, "arguments": args})

    finally:
        await client.close()


async def stream_chat_continue(messages: list[dict]) -> AsyncIterator[str]:
    """工具调用后继续流式生成（不带 tools 参数）"""
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
