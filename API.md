# TTS API

离线语音合成框架，支持 C++ 和 Python。

## 功能特性

- **非流式合成**: 文本到音频，阻塞直到完成
- **流式合成**: 边合成边播放，支持回调模式
- **双向流**: 边输入文本边合成（DuplexStream）
- **多语言**: 中文、英文、中英混合
- **参数调节**: 语速、音量、说话人

---

## C++ API

```cpp
namespace SpacemiT {

// =============================================================================
// AudioFormat - 音频格式
// =============================================================================

enum class AudioFormat {
    PCM,    // 原始 PCM 数据
    WAV,    // WAV 文件格式
    MP3,    // MP3 格式（预留）
    OGG,    // OGG 格式（预留）
};

// =============================================================================
// BackendType - 后端类型
// =============================================================================

enum class BackendType {
    MATCHA_ZH,      // 中文 (22050Hz)
    MATCHA_EN,      // 英文 (22050Hz)
    MATCHA_ZH_EN,   // 中英混合 (16000Hz)
    COSYVOICE,      // CosyVoice（预留）
    VITS,           // VITS（预留）
    PIPER,          // Piper TTS（预留）
    KOKORO,         // Kokoro 英文（兼容旧接口）
    KOKORO_EN,      // Kokoro v1.0 英文
    KOKORO_ZH,      // Kokoro v1.1 中文/中英混合
    CUSTOM,         // 自定义后端
};

// =============================================================================
// TtsConfig - 配置
// =============================================================================

struct TtsConfig {
    BackendType backend = BackendType::MATCHA_ZH;  // 后端类型
    std::string model;                  // 模型名称
    std::string model_dir;              // 模型目录路径
    std::string voice = "default";      // 音色名称
    int speaker_id = 0;                 // 说话人ID (多说话人模型)

    AudioFormat format = AudioFormat::WAV;  // 输出格式
    int sample_rate = 22050;            // 输出采样率 (Hz)
    int volume = 50;                    // 音量 [0, 100]

    float speech_rate = 1.0f;           // 语速 (>1.0快, <1.0慢)
    float pitch = 1.0f;                 // 音调

    int num_threads = 2;                // 推理线程数（Kokoro 预设为 4）
    bool enable_warmup = true;          // 启动时预热
    std::string provider = "auto";      // auto / cpu / spacemit

    // Matcha/Kokoro 共用的自定义发音词典
    std::vector<PronunciationEntry> lexicon;

    // 便捷构建方法
    static TtsConfig Preset(const std::string& name);
    static std::vector<std::string> AvailablePresets();

    // 链式配置
    TtsConfig withSpeed(float speed) const;
    TtsConfig withSpeaker(int id) const;
    TtsConfig withVolume(int vol) const;
};

// =============================================================================
// TtsEngineResult - 合成结果
// =============================================================================

class TtsEngineResult {
public:
    // 音频数据获取
    std::vector<uint8_t> GetAudioData() const;     // 字节格式
    std::vector<float> GetAudioFloat() const;      // float格式 [-1.0, 1.0]
    std::vector<int16_t> GetAudioInt16() const;    // int16格式

    // 元信息
    std::string GetTimestamp() const;              // 时间戳信息 (JSON)
    std::string GetResponse() const;               // 完整响应 (JSON)
    std::string GetRequestId() const;              // 请求 ID

    // 状态检查
    bool IsSuccess() const;                        // 是否成功
    std::string GetCode() const;                   // 错误码
    std::string GetMessage() const;                // 错误信息
    bool IsEmpty() const;                          // 是否为空
    bool IsSentenceEnd() const;                    // 是否句子结束（流式模式）

    // 性能指标
    int GetSampleRate() const;                     // 采样率 (Hz)
    int GetDurationMs() const;                     // 音频时长 (毫秒)
    int GetProcessingTimeMs() const;               // 处理时间 (毫秒)
    float GetRTF() const;                          // 实时率

    // 文件操作
    bool SaveToFile(const std::string& file_path) const;
};

// =============================================================================
// TtsResultCallback - 流式回调接口
// =============================================================================

/**
 * 回调调用顺序:
 *   StreamingCall() -> OnOpen() -> OnEvent()* -> OnComplete() -> OnClose()
 *   错误时: OnOpen() -> ... -> OnError() -> OnClose()
 */
class TtsResultCallback {
public:
    virtual void OnOpen() {}                                      // 会话开始
    virtual void OnEvent(std::shared_ptr<TtsEngineResult>) {}     // 收到音频块
    virtual void OnComplete() {}                                  // 合成完成
    virtual void OnError(const std::string& message) {}           // 发生错误
    virtual void OnClose() {}                                     // 会话关闭
};

// =============================================================================
// TtsEngine - 合成引擎
// =============================================================================

class TtsEngine {
public:
    // 双向流接口
    class DuplexStream {
    public:
        void SendText(const std::string& text);   // 发送文本
        void Complete();                          // 通知输入结束
        bool IsActive() const;                    // 检查流是否活跃
    };

    explicit TtsEngine(BackendType backend = BackendType::MATCHA_ZH,
                       const std::string& model_dir = "");
    explicit TtsEngine(const TtsConfig& config);

    // 非流式合成 (阻塞)
    std::shared_ptr<TtsEngineResult> Call(const std::string& text,
                                           const TtsConfig& config = TtsConfig());
    bool CallToFile(const std::string& text, const std::string& file_path);

    // 流式合成
    void StreamingCall(const std::string& text,
                       std::shared_ptr<TtsResultCallback> callback,
                       const TtsConfig& config = TtsConfig());
    std::shared_ptr<DuplexStream> StartDuplexStream(
        std::shared_ptr<TtsResultCallback> callback,
        const TtsConfig& config = TtsConfig());

    // 动态配置
    void SetSpeed(float speed);
    void SetSpeaker(int speaker_id);
    void SetVolume(int volume);
    TtsConfig GetConfig() const;

    // 辅助方法
    bool IsInitialized() const;
    std::string GetEngineName() const;
    BackendType GetBackendType() const;
    int GetNumSpeakers() const;
    int GetSampleRate() const;
};

}  // namespace SpacemiT
```

### C++ 示例

```cpp
#include "tts_service.h"
using namespace SpacemiT;

int main() {
    // 配置
    TtsConfig config = TtsConfig::Preset("matcha_zh");
    config.speech_rate = 1.0f;

    // 创建引擎
    auto engine = std::make_shared<TtsEngine>(config);

    // 文本合成
    auto result = engine->Call("你好世界");
    if (result && result->IsSuccess()) {
        std::cout << "时长: " << result->GetDurationMs() << "ms" << std::endl;
        std::cout << "RTF: " << result->GetRTF() << std::endl;
        result->SaveToFile("output.wav");
    }

    // 直接保存到文件
    engine->CallToFile("今天天气很好", "weather.wav");

    return 0;
}
```

### 流式回调示例

```cpp
#include "tts_service.h"
#include <iostream>

using namespace SpacemiT;

// 自定义回调
class MyCallback : public TtsResultCallback {
public:
    void OnOpen() override {
        std::cout << "开始合成" << std::endl;
    }

    void OnEvent(std::shared_ptr<TtsEngineResult> result) override {
        auto audio = result->GetAudioFloat();
        std::cout << "收到音频: " << audio.size() << " 样本, "
                  << result->GetDurationMs() << "ms" << std::endl;
        // 播放音频...
    }

    void OnComplete() override {
        std::cout << "合成完成" << std::endl;
    }

    void OnError(const std::string& message) override {
        std::cerr << "错误: " << message << std::endl;
    }

    void OnClose() override {
        std::cout << "会话关闭" << std::endl;
    }
};

int main() {
    // 创建引擎
    TtsConfig config = TtsConfig::Preset("matcha_zh");
    auto engine = std::make_shared<TtsEngine>(config);

    // 设置回调并启动流式合成
    auto callback = std::make_shared<MyCallback>();
    engine->StreamingCall("你好世界。今天天气很好。", callback);

    return 0;
}
```

### 双向流示例

```cpp
#include "tts_service.h"
#include <iostream>

using namespace SpacemiT;

int main() {
    auto engine = std::make_shared<TtsEngine>();
    auto callback = std::make_shared<MyCallback>();

    // 启动双向流
    auto stream = engine->StartDuplexStream(callback);

    // 边输入边合成
    stream->SendText("第一句话。");
    stream->SendText("第二句话。");
    stream->SendText("第三句话。");

    // 通知输入结束
    stream->Complete();

    return 0;
}
```

---

## Python API

```python
"""
Space TTS Python API
"""
import spacemit_tts
from spacemit_tts import Engine, Config, Result, BackendType, TtsCallback

# =============================================================================
# 快捷函数
# =============================================================================

def synthesize(text: str, model_dir: str = "~/.cache/models/tts/matcha-tts") -> Result:
    """
    合成文本 (一行代码)

    Args:
        text: 要合成的文本
        model_dir: 模型目录

    Returns:
        合成结果
    """

def synthesize_to_file(text: str, file_path: str, model_dir: str = "~/.cache/models/tts/matcha-tts") -> bool:
    """
    合成文本并保存到文件

    Args:
        text: 要合成的文本
        file_path: 输出文件路径
        model_dir: 模型目录

    Returns:
        是否成功
    """

# =============================================================================
# BackendType - 后端类型
# =============================================================================

class BackendType(Enum):
    MATCHA_ZH = ...     # 中文 (22050Hz)
    MATCHA_EN = ...     # 英文 (22050Hz)
    MATCHA_ZH_EN = ...  # 中英混合 (16000Hz)
    COSYVOICE = ...     # CosyVoice（预留）
    VITS = ...          # VITS（预留）
    PIPER = ...         # Piper TTS（预留）
    KOKORO = ...        # Kokoro 英文（兼容旧接口）
    KOKORO_EN = ...     # Kokoro v1.0 英文
    KOKORO_ZH = ...     # Kokoro v1.1 中文/中英混合

# =============================================================================
# Config - 配置
# =============================================================================

class Config:
    """TTS 配置"""

    def __init__(self, backend: BackendType = BackendType.MATCHA_ZH,
                 model_dir: str = "~/.cache/models/tts/matcha-tts"):
        """
        Args:
            backend: 后端类型
            model_dir: 模型目录路径
        """

    @staticmethod
    def preset(name: str) -> "Config":
        """通过预设名称创建配置（支持 'kokoro'、'kokoro_en'、'kokoro_zh'）"""

    @staticmethod
    def available_presets() -> list:
        """获取所有可用预设名称"""

    @property
    def speech_rate(self) -> float:
        """语速 (>1.0快, <1.0慢)"""

    @property
    def volume(self) -> int:
        """音量 [0, 100]"""

    @property
    def speaker_id(self) -> int:
        """说话人ID"""

    @property
    def sample_rate(self) -> int:
        """采样率 (Hz)"""

    # 链式配置
    def with_speed(self, speed: float) -> "Config": ...
    def with_speaker(self, speaker_id: int) -> "Config": ...
    def with_volume(self, volume: int) -> "Config": ...

# =============================================================================
# Result - 合成结果
# =============================================================================

class Result:
    """合成结果"""

    @property
    def audio_float(self) -> np.ndarray:
        """音频数据 (float32, [-1.0, 1.0])"""

    @property
    def audio_int16(self) -> np.ndarray:
        """音频数据 (int16)"""

    @property
    def audio_bytes(self) -> bytes:
        """音频数据 (bytes)"""

    @property
    def sample_rate(self) -> int:
        """采样率 (Hz)"""

    @property
    def duration_ms(self) -> int:
        """音频时长 (毫秒)"""

    @property
    def processing_time_ms(self) -> int:
        """处理时间 (毫秒)"""

    @property
    def rtf(self) -> float:
        """实时率 (处理时间 / 音频时长)"""

    @property
    def is_success(self) -> bool:
        """是否成功"""

    @property
    def message(self) -> str:
        """错误信息"""

    def save(self, file_path: str) -> bool:
        """保存到 WAV 文件"""

    def __bool__(self) -> bool: ...
    def __repr__(self) -> str: ...

# =============================================================================
# Engine - 合成引擎
# =============================================================================

class Engine:
    """
    TTS 引擎

    推荐使用 context manager:
        with Engine() as engine:
            result = engine.synthesize("你好")
    """

    def __init__(self, config: Optional[Config] = None):
        """
        Args:
            config: TTS 配置，不传则使用默认配置
        """

    def synthesize(self, text: str) -> Result:
        """
        合成文本 (阻塞)

        Args:
            text: 要合成的文本

        Returns:
            合成结果
        """

    def synthesize_to_file(self, text: str, file_path: str) -> bool:
        """
        合成文本并保存到文件

        Args:
            text: 要合成的文本
            file_path: 输出文件路径

        Returns:
            是否成功
        """

    def synthesize_streaming(self, text: str, callback: TtsCallback):
        """
        流式合成

        Args:
            text: 要合成的文本
            callback: TtsCallback 实例
        """

    def set_speed(self, speed: float):
        """设置语速"""

    def set_speaker(self, speaker_id: int):
        """设置说话人"""

    def set_volume(self, volume: int):
        """设置音量"""

    @property
    def config(self) -> Config:
        """获取当前配置"""

    @property
    def is_initialized(self) -> bool:
        """是否已初始化"""

    @property
    def sample_rate(self) -> int:
        """获取输出采样率 (Hz)"""

    # Context Manager
    def __enter__(self) -> "Engine": ...
    def __exit__(self, exc_type, exc_val, exc_tb): ...
```

### Python 示例

```python
import spacemit_tts
import numpy as np

# 快捷方式
spacemit_tts.synthesize_to_file("你好世界", "output.wav")

# Engine 类
config = spacemit_tts.Config.preset("matcha_zh")
config.speech_rate = 1.0

with spacemit_tts.Engine(config) as engine:
    # 文本合成
    result = engine.synthesize("你好世界")
    print(f"时长: {result.duration_ms}ms")
    print(f"RTF: {result.rtf:.3f}")
    result.save("output.wav")

    # 获取音频数组
    audio = result.audio_float  # numpy array
    print(f"样本数: {len(audio)}, 采样率: {result.sample_rate}Hz")

# 链式配置
config = spacemit_tts.Config.preset("matcha_en").with_speed(1.2).with_volume(80)

# 多语言
config_en = spacemit_tts.Config.preset("matcha_en")      # 英文
config_zh_en = spacemit_tts.Config.preset("matcha_zh_en")  # 中英混合
```

---

## 流式合成

Python 流式合成使用回调模式，边合成边播放。

### TtsCallback

```python
from spacemit_tts import TtsCallback

class MyCallback(TtsCallback):
    """自定义回调处理类"""

    def on_open(self) -> None:
        """合成会话开始"""
        print("开始合成")

    def on_event(self, result) -> None:
        """
        收到合成结果

        Args:
            result: 包含音频数据的结果对象
        """
        audio = result.audio_float
        print(f"收到音频: {result.get_duration_ms()}ms, RTF={result.get_rtf():.3f}")
        # 播放音频...

    def on_complete(self) -> None:
        """合成完成"""
        print("合成完成")

    def on_error(self, message: str) -> None:
        """发生错误"""
        print(f"错误: {message}")

    def on_close(self) -> None:
        """会话关闭"""
        print("会话关闭")
```

### 流式合成示例

```python
import spacemit_tts
from spacemit_tts import TtsCallback
import spacemit_audio
from spacemit_audio import AudioPlayer

class StreamingCallback(TtsCallback):
    def __init__(self, player):
        super().__init__()
        self.player = player

    def on_event(self, result):
        # 直接播放收到的音频
        audio_bytes = result.audio_bytes
        self.player.write(audio_bytes)
        print(f"播放: {result.get_duration_ms()}ms")

# 配置音频播放 (22050Hz, mono)
spacemit_audio.init(sample_rate=22050, channels=1)

with AudioPlayer() as player:
    player.start()

    with spacemit_tts.Engine() as engine:
        callback = StreamingCallback(player)
        engine.synthesize_streaming("你好世界。今天天气很好。", callback)
```

### 预置回调类

```python
from spacemit_tts import PrintCallback, SaveCallback, CollectCallback

# PrintCallback - 打印调试信息
callback = PrintCallback()
engine.synthesize_streaming("你好", callback)
# 输出:
# [TTS] Session opened
# [TTS] Audio: 2304ms, RTF=0.023
# [TTS] Synthesis completed
# [TTS] Session closed

# SaveCallback - 保存音频块到文件
callback = SaveCallback(output_dir="output", prefix="chunk")
engine.synthesize_streaming("你好世界", callback)
# 创建 output/chunk_000.wav, output/chunk_001.wav, ...

# CollectCallback - 收集所有结果
callback = CollectCallback()
engine.synthesize_streaming("你好世界", callback)
for i, result in enumerate(callback.results):
    print(f"Chunk {i}: {result.duration_ms}ms")
total_duration = callback.get_total_duration_ms()
```

---

## 数据格式

- **采样率**:
  - MATCHA_ZH: 22050 Hz
  - MATCHA_EN: 22050 Hz
  - MATCHA_ZH_EN: 16000 Hz
- **声道**: 单声道 (mono)
- **格式**:
  - C++: `std::vector<float>` 或 `std::vector<int16_t>`
  - Python: `np.ndarray` (float32 或 int16)
- **范围**:
  - float32: [-1.0, 1.0]
  - int16: [-32768, 32767]

```python
# float32 -> PCM16 bytes
audio_bytes = (audio_float * 32767).astype(np.int16).tobytes()

# PCM16 bytes -> float32
audio_float = np.frombuffer(audio_bytes, dtype=np.int16).astype(np.float32) / 32768.0
```

---

## 支持的后端

| 后端 | 语言 | 采样率 | 状态 |
|------|------|--------|------|
| MATCHA_ZH | 中文 | 22050Hz | ✓ |
| MATCHA_EN | 英文 | 22050Hz | ✓ |
| MATCHA_ZH_EN | 中英混合 | 16000Hz | ✓ |
| COSYVOICE | 多语言 | - | 预留 |
| VITS | 多语言 | - | 预留 |
| PIPER | 多语言 | - | 预留 |
| KOKORO / KOKORO_EN | 英文 | 24000Hz | 已实现 |
| KOKORO_ZH | 中文/中英混合 | 24000Hz | 已实现 |
