"""
TTS 模块：阿里云 CosyVoice v2 流式语音合成

输入：文字字符串
输出：16kHz 16bit mono PCM 音频字节流（与 ESP32 AudioManager 直接兼容）

用法：
    async for chunk in tts.synthesize_stream("你好"):
        await websocket.send(chunk)

工作原理：
    DashScope TTS SDK 使用回调模式（callback），运行在独立线程中。
    本模块通过 asyncio.Queue + call_soon_threadsafe 将其桥接到 async/await 代码。
"""
import asyncio
import os
import sys
import threading
from typing import AsyncIterator

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config


async def synthesize_stream(text: str) -> AsyncIterator[bytes]:
    """流式合成语音，逐块 yield PCM 音频数据

    每次调用建立一个 TTS 会话，适合逐句调用（Phase 1）。
    """
    import dashscope
    from dashscope.audio.tts_v2 import AudioFormat, ResultCallback, SpeechSynthesizer

    dashscope.api_key = config.DASHSCOPE_API_KEY

    queue: asyncio.Queue = asyncio.Queue()
    loop = asyncio.get_running_loop()

    class _Callback(ResultCallback):
        def on_open(self) -> None:
            pass

        def on_close(self) -> None:
            pass

        def on_complete(self) -> None:
            # TTS 全部合成完毕，向队列发送结束信号
            loop.call_soon_threadsafe(queue.put_nowait, None)

        def on_error(self, message: str) -> None:
            loop.call_soon_threadsafe(queue.put_nowait, RuntimeError(f"TTS 错误: {message}"))

        def on_data(self, data: bytes) -> None:
            # 将音频块安全地推入异步队列
            loop.call_soon_threadsafe(queue.put_nowait, data)

    syn = SpeechSynthesizer(
        model=config.TTS_MODEL,
        voice=config.TTS_VOICE,
        format=AudioFormat.PCM_16000HZ_MONO_16BIT,  # 直接输出 16kHz 16bit，无需重采样
        callback=_Callback(),
    )

    def _run() -> None:
        """在后台线程中启动 TTS（streaming_call 可能阻塞）"""
        syn.streaming_call(text)
        syn.streaming_complete()

    thread = threading.Thread(target=_run, daemon=True)
    thread.start()

    # 从队列读取音频块，直到收到 None（结束）
    while True:
        item = await queue.get()
        if item is None:
            break
        if isinstance(item, RuntimeError):
            raise item
        yield item

    thread.join(timeout=10)
