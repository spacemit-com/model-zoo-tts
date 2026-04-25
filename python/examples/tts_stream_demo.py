#!/usr/bin/env python3
"""
流式 TTS 演示程序

功能:
1. 模拟 LLM 流式输出 - 按标点符号切分文本
2. 流式 TTS 合成 - 逐句合成
3. 音频队列 - 存放合成的音频块
4. 音频重采样 - 支持任意采样率上采样到48kHz
5. 声道转换 - 支持单/双声道输出
6. 实时播放 - 使用 spacemit_audio 播放

用法:
  python streaming_demo.py                           # 使用默认文本
  python streaming_demo.py -p "文本"                 # 自定义文本
  python streaming_demo.py -l zh-en                  # 指定语言
  python streaming_demo.py --output-rate 48000       # 输出48kHz
  python streaming_demo.py --channels 2              # 双声道输出
  python streaming_demo.py --no-play                 # 不播放，只测试接口
"""

import argparse
import queue
import threading
import time
from dataclasses import dataclass
from typing import List

import numpy as np

import spacemit_tts

# 尝试导入 spacemit_audio，如果不可用则禁用播放
try:
    import spacemit_audio
    from spacemit_audio import AudioPlayer
    HAS_SPACEMIT_AUDIO = True
except ImportError:
    HAS_SPACEMIT_AUDIO = False


# =============================================================================
# 音频块数据结构
# =============================================================================

@dataclass
class AudioChunk:
    """音频块"""
    samples: np.ndarray  # int16 音频数据
    sample_rate: int     # 采样率
    sentence_index: int  # 句子索引
    is_end_marker: bool = False  # 是否为结束标记


# =============================================================================
# 文本切分器 - 按标点符号切分
# =============================================================================

class TextSplitter:
    """按标点符号切分文本为句子"""

    # 中文句末标点
    CN_PUNCTS = {"。", "！", "？", "；"}
    # 英文句末标点
    EN_PUNCTS = {".","!","?",";"}

    def split(self, text: str) -> List[str]:
        """
        按标点符号切分文本

        Args:
            text: 输入文本

        Returns:
            句子列表
        """
        sentences = []
        buffer = ""

        for char in text:
            buffer += char

            # 检查是否为句末标点
            if char in self.CN_PUNCTS or char in self.EN_PUNCTS:
                if buffer.strip():
                    sentences.append(buffer)
                buffer = ""

        # 处理剩余文本
        if buffer.strip():
            sentences.append(buffer)

        return sentences


# =============================================================================
# 音频处理工具
# =============================================================================

def resample_audio(samples: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    """
    将音频重采样到目标采样率 (线性插值)

    Args:
        samples: 输入音频 (int16)
        src_rate: 源采样率
        dst_rate: 目标采样率

    Returns:
        重采样后的音频 (int16)
    """
    if src_rate == dst_rate or len(samples) == 0:
        return samples

    # 计算输出长度
    ratio = dst_rate / src_rate
    output_length = int(len(samples) * ratio)

    # 转为 float 进行插值
    samples_float = samples.astype(np.float32) / 32768.0

    # 线性插值
    x_old = np.linspace(0, 1, len(samples_float))
    x_new = np.linspace(0, 1, output_length)
    resampled = np.interp(x_new, x_old, samples_float)

    # 转回 int16
    resampled = np.clip(resampled, -1.0, 1.0)
    return (resampled * 32767).astype(np.int16)


def mono_to_stereo(samples: np.ndarray) -> np.ndarray:
    """
    将单声道音频转换为双声道

    Args:
        samples: 单声道音频 (int16)

    Returns:
        双声道音频 (int16), shape: (N, 2)
    """
    return np.column_stack((samples, samples))


# =============================================================================
# 合成线程
# =============================================================================

def synthesis_thread(
    engine: spacemit_tts.Engine,
    sentences: List[str],
    audio_queue: queue.Queue,
    delay_ms: int
):
    """
    合成线程 - 模拟 LLM 输出并逐句合成

    Args:
        engine: TTS 引擎
        sentences: 句子列表
        audio_queue: 音频队列
        delay_ms: LLM 模拟延迟 (毫秒/字符)
    """
    print(f"[合成] 开始合成, 共 {len(sentences)} 句")

    for idx, sentence in enumerate(sentences, 1):
        # 计算字符数 (用于模拟 LLM 延迟)
        char_count = len(sentence)

        print(f"[LLM] 生成第 {idx} 句 ({char_count} 字): {sentence}")

        # 模拟 LLM 生成延迟
        if delay_ms > 0:
            time.sleep(delay_ms * char_count / 1000.0)

        # 合成
        start_time = time.time()
        result = engine.synthesize(sentence)
        elapsed = int((time.time() - start_time) * 1000)

        if result and result.is_success:
            audio_int16 = np.array(result.audio_int16)
            sample_rate = result.sample_rate
            duration_ms = result.duration_ms
            rtf = result.rtf

            print(f"[合成] 第 {idx} 句完成: {len(audio_int16)} 样本, "
                  f"{duration_ms} ms, 耗时 {elapsed} ms, RTF={rtf:.3f}")

            # 放入队列
            chunk = AudioChunk(
                samples=audio_int16,
                sample_rate=sample_rate,
                sentence_index=idx
            )
            audio_queue.put(chunk)
        else:
            message = result.message if result else "unknown error"
            print(f"[合成] 第 {idx} 句失败: {message}")

    # 发送结束标记
    end_chunk = AudioChunk(
        samples=np.array([], dtype=np.int16),
        sample_rate=0,
        sentence_index=-1,
        is_end_marker=True
    )
    audio_queue.put(end_chunk)

    print("[合成] 全部完成")


# =============================================================================
# 播放线程
# =============================================================================

def playback_thread(
    audio_queue: queue.Queue,
    output_rate: int,
    channels: int,
    enable_play: bool
):
    """
    播放线程 - 从队列获取音频并播放

    Args:
        audio_queue: 音频队列
        output_rate: 输出采样率
        channels: 输出声道数 (1 或 2)
        enable_play: 是否启用播放
    """
    if enable_play and not HAS_SPACEMIT_AUDIO:
        print("[播放] 警告: spacemit_audio 未安装，禁用播放")
        print("       安装: cd components/multimedia/audio/python && pip install -e .")
        enable_play = False

    player = None
    if enable_play:
        # 初始化 spacemit_audio
        spacemit_audio.init(
            sample_rate=output_rate,
            channels=channels,
        )
        player = AudioPlayer()
        player.start()
        print(f"[播放] 播放器已启动, 采样率: {output_rate} Hz, 声道: {channels}")

    played_count = 0

    while True:
        try:
            chunk = audio_queue.get(timeout=10.0)
        except queue.Empty:
            print("[播放] 队列超时，退出")
            break

        # 检查结束标记
        if chunk.is_end_marker:
            print("[播放] 收到结束标记")
            break

        if len(chunk.samples) == 0:
            continue

        played_count += 1
        duration_ms = len(chunk.samples) * 1000 // chunk.sample_rate
        print(f"[播放] 播放第 {chunk.sentence_index} 句: "
              f"{len(chunk.samples)} 样本 @ {chunk.sample_rate} Hz, {duration_ms} ms")

        if enable_play and player:
            # 重采样
            resampled = resample_audio(chunk.samples, chunk.sample_rate, output_rate)
            print(f"[播放] 重采样: {chunk.sample_rate} -> {output_rate} Hz "
                  f"({len(resampled)} 样本)")

            # 声道转换
            if channels == 2:
                final_audio = mono_to_stereo(resampled)
                print(f"[播放] 转换为双声道 ({len(final_audio)} 帧)")
            else:
                final_audio = resampled

            # 转换为 bytes 并播放 (阻塞)
            audio_bytes = final_audio.tobytes()
            player.write(audio_bytes)
        else:
            # 不播放时模拟短延迟
            time.sleep(0.05)

    if player:
        player.stop()
        player.close()

    print(f"[播放] 播放完成, 共 {played_count} 句")


# =============================================================================
# 主函数
# =============================================================================

def parse_language(lang: str) -> spacemit_tts.Config:
    """解析语言参数并返回配置"""
    if lang == "en":
        return spacemit_tts.Config.preset("matcha_en")
    elif lang == "zh":
        return spacemit_tts.Config.preset("matcha_zh")
    elif lang == "kokoro":
        return spacemit_tts.Config.preset("kokoro")
    else:  # zh-en
        return spacemit_tts.Config.preset("matcha_zh_en")


def main():
    parser = argparse.ArgumentParser(description="流式 TTS 演示程序")
    parser.add_argument("-p", "--text", type=str, default=None,
                        help="自定义文本")
    parser.add_argument("-l", "--language", type=str, default="zh-en",
                        choices=["zh", "en", "zh-en", "kokoro"],
                        help="语言: zh, en, zh-en (默认: zh-en)")
    parser.add_argument("--output-rate", type=int, default=48000,
                        help="输出采样率 (默认: 48000)")
    parser.add_argument("--channels", type=int, default=1, choices=[1, 2],
                        help="输出声道数: 1=单声道, 2=双声道 (默认: 1)")
    parser.add_argument("--no-play", action="store_true",
                        help="不播放音频")
    parser.add_argument("--delay", type=int, default=5,
                        help="模拟 LLM 输出延迟 (默认: 5 ms/字符)")

    args = parser.parse_args()

    # 默认的中英混合长文本
    if args.text:
        text = args.text
    else:
        text = (
            "大家好，今天我们来讨论一下人工智能的发展。"
            "AI技术在recent years取得了remarkable progress。"
            "特别是在Natural Language Processing领域，"
            "像ChatGPT这样的large language models已经能够进行流畅的对话。"
            "这些技术正在改变我们的生活方式。"
            "比如说，语音助手可以帮助我们控制smart home devices。"
            "Machine Learning可以帮助医生诊断疾病。"
            "未来，AI将会更加intelligent，更加helpful。"
            "让我们一起期待这个exciting的未来吧！"
        )

    enable_play = not args.no_play

    print("=" * 60)
    print("        流式 TTS 演示程序")
    print("=" * 60)
    print(f"语言: {args.language}")
    print(f"输出采样率: {args.output_rate} Hz")
    print(f"输出声道: {args.channels}")
    print(f"播放: {'是' if enable_play else '否'}")
    print(f"LLM 延迟: {args.delay} ms/字符")
    print("=" * 60)
    print()

    # 创建 TTS 引擎
    print("初始化 TTS 引擎...")
    config = parse_language(args.language)
    engine = spacemit_tts.Engine(config)

    print(f"引擎: {engine.engine_name}")
    print(f"采样率: {engine.sample_rate} Hz")
    print()

    # 切分文本
    splitter = TextSplitter()
    sentences = splitter.split(text)

    print("=" * 60)
    print(f"输入文本: {text}")
    print(f"切分为 {len(sentences)} 句")
    print("=" * 60)
    print()

    # 创建音频队列
    audio_queue = queue.Queue()

    # 启动线程
    synth_thread = threading.Thread(
        target=synthesis_thread,
        args=(engine, sentences, audio_queue, args.delay)
    )

    play_thread = threading.Thread(
        target=playback_thread,
        args=(audio_queue, args.output_rate, args.channels, enable_play)
    )

    # 先启动播放线程，再启动合成线程
    play_thread.start()
    synth_thread.start()

    # 等待线程结束
    synth_thread.join()
    play_thread.join()

    print()
    print("=" * 60)
    print("        演示完成")
    print("=" * 60)


if __name__ == "__main__":
    main()
