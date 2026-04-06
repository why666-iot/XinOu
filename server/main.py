"""
心偶 AI 情感陪伴机器人 - WebSocket 服务器 (Phase 2: 场景分类 + 提示词路由)

启动方法（在 server/ 目录下运行）：
    cd server
    set DASHSCOPE_API_KEY=sk-xxx
    set LLM_API_KEY=sk-xxx
    python main.py

ESP32 连接地址：ws://<本机局域网IP>:8888
（在 main/main.cc 的 WS_URI_DEFAULT 中填入此地址）

通信协议（与现有 ESP32 固件完全兼容，无需修改硬件端代码）：
    ESP32 → Server : binary  - 16kHz 16bit mono PCM 音频块
    ESP32 → Server : text    - JSON 控制消息（recording_started / recording_ended 等）
    Server → ESP32 : binary  - TTS 生成的 PCM 音频块（流式）
    Server → ESP32 : ping    - 音频流结束信号（ESP32 收到后停止等待，继续下一轮）
"""
import asyncio
import json
import os
import socket
import struct
import sys
from datetime import datetime

import websockets

# 把 server/ 目录加入路径，让子模块的绝对导入正常工作
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import config
from core import orchestrator

# ── Debug 录音保存 ─────────────────────────────────────────
DEBUG_AUDIO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "debug_audio")
os.makedirs(DEBUG_AUDIO_DIR, exist_ok=True)


def _save_debug_wav(pcm_bytes: bytes, asr_text: str) -> str:
    """将 PCM 数据保存为 WAV 文件，返回文件路径"""
    # 文件名：时间戳_ASR结果前10字
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_text = (asr_text or "无识别")[:10].replace("/", "_").replace("\\", "_")
    filename = f"{ts}_{safe_text}.wav"
    filepath = os.path.join(DEBUG_AUDIO_DIR, filename)

    # 写 WAV 头 + PCM 数据
    num_channels = 1
    sample_width = 2  # 16-bit
    data_size = len(pcm_bytes)
    with open(filepath, "wb") as f:
        # RIFF header
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        # fmt chunk
        f.write(b"fmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, num_channels, config.SAMPLE_RATE,
                            config.SAMPLE_RATE * num_channels * sample_width,
                            num_channels * sample_width, sample_width * 8))
        # data chunk
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        f.write(pcm_bytes)

    return filepath


async def handle_connection(websocket) -> None:
    """处理单个 ESP32 WebSocket 连接（每个连接独立协程）"""
    client_ip = websocket.remote_address[0]
    print(f"\n[+] 连接：{client_ip}")

    audio_buffer = bytearray()
    recording = False
    history: list[dict] = []      # 本连接的对话历史
    prev_scene = ""               # 上轮场景 ID（用于短语句场景继承）
    classify_context: list[str] = []  # 场景分类上下文累积（跨轮共享）

    try:
        async for message in websocket:
            # ── 二进制帧：PCM 音频数据 ────────────────────────
            if isinstance(message, bytes):
                if recording:
                    audio_buffer.extend(message)

            # ── 文本帧：JSON 控制消息 ─────────────────────────
            elif isinstance(message, str):
                try:
                    data = json.loads(message)
                except json.JSONDecodeError:
                    continue

                event = data.get("event", "")

                if event == "wake_word_detected":
                    print(f"[{client_ip}] 唤醒词检测到")

                elif event == "recording_started":
                    print(f"[{client_ip}] 开始录音")
                    recording = True
                    audio_buffer = bytearray()

                elif event == "recording_ended":
                    recording = False
                    pcm = bytes(audio_buffer)
                    audio_buffer = bytearray()

                    duration = len(pcm) / (config.SAMPLE_RATE * 2)
                    print(f"[{client_ip}] 录音结束，时长 {duration:.2f}s，大小 {len(pcm)} bytes")

                    # ── 全链路处理：ASR → 分类 → LLM → TTS ──
                    user_text, scene, reply_parts, audio_gen = await orchestrator.process(
                        pcm, history, prev_scene, classify_context
                    )

                    # 保存 debug 音频
                    debug_path = _save_debug_wav(pcm, user_text)
                    print(f"[DEBUG] 录音已保存: {debug_path}")

                    if user_text:
                        prev_scene = scene  # 记住本轮场景
                        # 流式发送 TTS 音频给 ESP32
                        chunk_count = 0
                        async for chunk in audio_gen:
                            await websocket.send(chunk)
                            chunk_count += 1

                        # 发送 ping 作为音频结束信号
                        # ESP32 的 AudioManager 收到 ping 后触发 finishStreamingPlayback()
                        await websocket.ping()

                        # 更新对话历史
                        full_reply = "".join(reply_parts)
                        history.append({"role": "user", "content": user_text})
                        history.append({"role": "assistant", "content": full_reply})
                        print(f"[完成] 发送 {chunk_count} 个音频块，回复：{full_reply}")
                    else:
                        # ASR 未识别到语音，发送静音块 + ping 让 ESP32 继续工作
                        # 必须发送至少一个音频块，否则 ESP32 的 isStreamingActive() 为 false，ping 会被忽略
                        print(f"[{client_ip}] ASR 未识别到语音，发送静音信号")
                        silence = b'\x00' * 3200  # 0.1秒静音 (16000Hz * 2bytes * 0.1s)
                        await websocket.send(silence)
                        await websocket.ping()

                elif event == "recording_cancelled":
                    print(f"[{client_ip}] 录音取消（本地命令词处理）")
                    recording = False
                    audio_buffer = bytearray()

    except websockets.exceptions.ConnectionClosed:
        print(f"[-] 断开：{client_ip}")
    except Exception as e:
        print(f"[!] 错误 {client_ip}：{e}")
        import traceback
        traceback.print_exc()


def get_local_ip() -> str:
    """获取本机局域网 IP（用于提示用户填写 WS_URI）"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


async def main() -> None:
    print("=" * 55)
    print("  心偶 WebSocket 服务器  (Phase 2: 场景分类 + 提示词路由)")
    print("=" * 55)

    # 检查 API keys
    missing = []
    if not config.DASHSCOPE_API_KEY:
        missing.append("DASHSCOPE_API_KEY（ASR + TTS）")
    if not config.LLM_API_KEY:
        missing.append("LLM_API_KEY")
    if missing:
        for key in missing:
            print(f"  ⚠  未设置环境变量：{key}")

    local_ip = get_local_ip()
    print(f"\n  ESP32 填写地址：ws://{local_ip}:{config.WS_PORT}")
    print(f"  ASR : {config.ASR_MODEL}")
    print(f"  LLM : {config.LLM_MODEL}  ({config.LLM_BASE_URL})")
    print(f"  TTS : {config.TTS_MODEL} / {config.TTS_VOICE}")
    print("=" * 55)
    print("  等待 ESP32 连接...\n")

    async with websockets.serve(handle_connection, config.WS_HOST, config.WS_PORT):
        await asyncio.Future()  # 永久运行


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n服务器已停止")
