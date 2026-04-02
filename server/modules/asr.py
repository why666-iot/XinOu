"""
ASR 模块：阿里云 Paraformer 语音识别

输入：16kHz 16bit mono PCM 字节
输出：识别文字字符串

用法：
    text = await asr.transcribe(pcm_bytes)
"""
import asyncio
import os
import sys
import tempfile
import wave

# 确保 server/ 目录在路径中（被 orchestrator 导入时生效）
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config


def _recognize_sync(wav_path: str) -> str:
    """同步 ASR 调用（在线程中运行，避免阻塞事件循环）"""
    import dashscope
    from dashscope.audio.asr import Recognition, RecognitionCallback

    dashscope.api_key = config.DASHSCOPE_API_KEY

    recognizer = Recognition(
        model=config.ASR_MODEL,
        format="wav",
        sample_rate=config.SAMPLE_RATE,
        callback=RecognitionCallback(),
    )
    result = recognizer.call(wav_path)

    print(f"[ASR debug] status={result.status_code} code={result.code} message={result.message} output={result.output}")

    if result.status_code != 200:
        raise RuntimeError(f"ASR 错误 {result.code}: {result.message}")

    if not result.output:
        return ""

    sentences = result.output.get("sentence", [])
    return "".join(s.get("text", "") for s in sentences).strip()


async def transcribe(pcm_bytes: bytes) -> str:
    """将 PCM 音频字节识别为文字

    少于 0.2 秒的音频直接返回空字符串。
    """
    min_bytes = int(config.SAMPLE_RATE * 2 * 0.2)  # 0.2s * 2 bytes/sample
    if len(pcm_bytes) < min_bytes:
        return ""

    # 写入临时 WAV 文件（DashScope SDK 需要文件路径）
    fd, wav_path = tempfile.mkstemp(suffix=".wav")
    os.close(fd)
    try:
        with wave.open(wav_path, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)  # 16-bit = 2 bytes
            wf.setframerate(config.SAMPLE_RATE)
            wf.writeframes(pcm_bytes)

        return await asyncio.to_thread(_recognize_sync, wav_path)
    finally:
        os.unlink(wav_path)
