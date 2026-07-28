#!/usr/bin/env python3
"""
Simple TTS Demo

Demonstrates basic text-to-speech synthesis.
"""

import spacemit_tts


def main():
    print("=" * 60)
    print("  Simple TTS Demo")
    print("=" * 60)
    print()

    # Create engine with default configuration (Chinese)
    print("Creating TTS engine (Chinese)...")
    engine = spacemit_tts.Engine()
    print(f"Engine: {engine.engine_name}")
    print(f"Sample rate: {engine.sample_rate} Hz")
    print()

    # Synthesize Chinese text
    text = "你好世界，这是一个语音合成测试。"
    print(f"Synthesizing: {text}")
    result = engine.synthesize(text)

    if result:
        print("✓ Success!")
        print(f"  Duration: {result.duration_ms} ms")
        print(f"  RTF: {result.rtf:.3f}")
        print(f"  Audio samples: {len(result.audio_float)}")

        # Save to file
        output_file = "output_chinese.wav"
        result.save(output_file)
        print(f"  Saved to: {output_file}")
    else:
        print(f"✗ Failed: {result.message}")
    print()

    # English synthesis
    print("Creating TTS engine (English)...")
    config = spacemit_tts.Config.preset("matcha_en")
    engine_en = spacemit_tts.Engine(config)
    print(f"Engine: {engine_en.engine_name}")
    print()

    text_en = "Hello world, this is a text-to-speech test."
    print(f"Synthesizing: {text_en}")
    result_en = engine_en.synthesize(text_en)

    if result_en:
        print("✓ Success!")
        print(f"  Duration: {result_en.duration_ms} ms")
        print(f"  RTF: {result_en.rtf:.3f}")

        output_file = "output_english.wav"
        result_en.save(output_file)
        print(f"  Saved to: {output_file}")
    else:
        print(f"✗ Failed: {result_en.message}")
    print()

    # Bilingual synthesis
    print("Creating TTS engine (Bilingual)...")
    config_bilingual = spacemit_tts.Config.preset("matcha_zh_en")
    engine_bilingual = spacemit_tts.Engine(config_bilingual)
    print(f"Engine: {engine_bilingual.engine_name}")
    print()

    text_bilingual = "你好 Hello，这是 bilingual test。"
    print(f"Synthesizing: {text_bilingual}")
    result_bilingual = engine_bilingual.synthesize(text_bilingual)

    if result_bilingual:
        print("✓ Success!")
        print(f"  Duration: {result_bilingual.duration_ms} ms")
        print(f"  RTF: {result_bilingual.rtf:.3f}")

        output_file = "output_bilingual.wav"
        result_bilingual.save(output_file)
        print(f"  Saved to: {output_file}")
    else:
        print(f"✗ Failed: {result_bilingual.message}")
    print()

    # Kokoro synthesis (24000Hz)
    print("Creating TTS engine (Kokoro)...")
    config_kokoro = spacemit_tts.Config.preset("kokoro").with_provider("spacemit")
    engine_kokoro = spacemit_tts.Engine(config_kokoro)
    print(f"Engine: {engine_kokoro.engine_name}")
    print(f"Sample rate: {engine_kokoro.sample_rate} Hz")
    print()

    text_kokoro = "你好，我是Kokoro语音合成引擎。"
    print(f"Synthesizing: {text_kokoro}")
    result_kokoro = engine_kokoro.synthesize(text_kokoro)

    if result_kokoro:
        print("✓ Success!")
        print(f"  Duration: {result_kokoro.duration_ms} ms")
        print(f"  RTF: {result_kokoro.rtf:.3f}")

        output_file = "output_kokoro.wav"
        result_kokoro.save(output_file)
        print(f"  Saved to: {output_file}")
    else:
        print(f"✗ Failed: {result_kokoro.message}")
    print()

    # Adjust speed
    print("Synthesizing with different speeds...")
    engine.set_speed(0.8)
    result_slow = engine.synthesize("这是慢速语音。")
    result_slow.save("output_slow.wav")
    print(f"  Slow (0.8x): {result_slow.duration_ms} ms")

    engine.set_speed(1.5)
    result_fast = engine.synthesize("这是快速语音。")
    result_fast.save("output_fast.wav")
    print(f"  Fast (1.5x): {result_fast.duration_ms} ms")
    print()

    # Quick synthesis using utility function
    print("Quick synthesis using utility function...")
    spacemit_tts.synthesize_to_file("这是快速合成", "output_quick.wav")
    print("  Saved to: output_quick.wav")
    print()

    print("=" * 60)
    print("  Demo completed successfully!")
    print("=" * 60)


if __name__ == "__main__":
    main()
