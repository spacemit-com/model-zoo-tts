# TTS 组件

## 1. 项目简介

本组件为通用 TTS 封装，提供统一的 C++ 接口与 Python 绑定，支持本地多种合成引擎，便于集成到 AI Agent 等应用中。当前已支持 Matcha-TTS、Kokoro 等（本地 ONNX），接口可扩展其他后端。功能特性如下：

| 类别     | 支持                                                                 |
| -------- | -------------------------------------------------------------------- |
| 部署方式 | **本地**（ONNX 推理）                                                |
| 合成方式 | 文本阻塞合成 `Call()`、`CallToFile()`；流式合成 `StreamingCall()`、`StartDuplexStream()` |
| 后端     | MATCHA_ZH / MATCHA_EN / MATCHA_ZH_EN / KOKORO                         |
| 语言     | 中文、英文、中英混合                                                 |
| 接口     | C++（`include/tts_service.h`）、Python（`spacemit_tts`）             |

支持的后端：

| 后端 | 语言 | 采样率 | 状态 |
|------|------|--------|------|
| MATCHA_ZH | 中文 | 22050Hz | ✓ |
| MATCHA_EN | 英文 | 22050Hz | ✓ |
| MATCHA_ZH_EN | 中英混合 | 16000Hz | ✓ |
| KOKORO | 中文/英文 | 24000Hz | ✓ |

## 2. 验证模型

按以下顺序完成依赖安装、模型准备与示例运行。

### 2.1. 安装依赖

- **编译环境**：CMake ≥ 3.16，C++17 编译器（GCC/Clang/MSVC）。
- **必选**：libsndfile、libfftw3、espeak-ng；ONNX Runtime（需手动安装或指定路径）。

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake \
  libsndfile1-dev libfftw3-dev espeak-ng
```

**指定 ONNX Runtime 路径（可选）：**
```bash
cmake -B build -S . \
  -DONNXRUNTIME_INCLUDE_DIR=/path/to/onnxruntime/include \
  -DONNXRUNTIME_LIB="/path/to/onnxruntime/lib/libonnxruntime.so"
```

**可选：**
- **Python 绑定**：`pip install pybind11` 或 `apt install python3-pybind11`
- **流式示例（C++）**：需 audio 组件 + PortAudio，`apt install portaudio19-dev`。SDK 编译时默认开启

### 2.2. 下载模型

使用 Matcha-TTS 等时需将模型放到默认路径 **`~/.cache/models/tts/matcha-tts/`**（或各后端约定路径）；首次运行可自动下载，也可从镜像提前下载。

**模型源：**
- **进迭时空镜像（推荐）**：<https://archive.spacemit.com/spacemit-ai/model_zoo/tts/>  
  提供 TTS 模型压缩包，下载后解压到默认目录即可，例如：
  ```bash
  mkdir -p ~/.cache/models/tts
  cd ~/.cache/models/tts
  wget https://archive.spacemit.com/spacemit-ai/model_zoo/tts/matcha-tts.tar.gz
  tar -xzf matcha-tts.tar.gz
  ```
  解压后需得到与组件预期一致的目录结构（具体以各后端文档为准）。
- **其他渠道**：从各后端官方或内部渠道获取模型文件，解压/拷贝至上述目录。

### 2.3. 测试

本节提供示例程序的编译与运行方式，便于开发者快速验证效果。使用前需先按下列两种方式之一完成编译，再运行对应示例。

- **在 SDK 中验证**（2.3.1）：在已拉取的 SpacemiT Robot SDK 工程内用 `mm` 编译，产物部署到 `output/staging`，适合整机集成或与 ASR、LLM 等模块联调。
- **独立构建下验证**（2.3.2）：在 TTS 组件目录下用 CMake 本地编译，不依赖完整 SDK，适合快速体验或在不使用 repo 的环境下使用。

#### 2.3.1. 在 SDK 中验证

**编译**：本组件已纳入 SpacemiT Robot SDK 时，在 SDK 根目录下执行。SDK 拉取与初始化见 [SpacemiT Robot SDK Manifest](https://github.com/spacemit-robotics/manifest)（使用 repo 时需先完成 `repo init`、`repo sync` 等）。

```bash
source build/envsetup.sh
cd components/model_zoo/tts
mm
```

构建产物会安装到 `output/staging`。

**运行**：运行前在 SDK 根目录执行 `source build/envsetup.sh`，使 PATH 与库路径指向 `output/staging`，然后可执行：

**C++ 简单合成：**
```bash
tts_file_demo
tts_file_demo -p "你好世界" -l matcha:zh-en
```

**Python 文件合成**（需已安装 Python 包或设置 PYTHONPATH 指向 SDK 构建产物）：
```bash
python python/examples/tts_file_demo.py
```

**流式合成**（SDK 编译时默认已开启）：
```bash
tts_stream_demo -p "自定义文本"
tts_stream_demo -l # 查看设备
tts_stream_demo -o 0 --output-rate 48000 --channels 2 -e matcha:zh-en # 默认流式tts体验
```

**Python 流式合成**（需已安装 `spacemit_tts`；播放时还需已安装 `spacemit_audio`）：

```bash
python python/examples/tts_stream_demo.py --no-play
python python/examples/tts_stream_demo.py -p "自定义文本"
```

#### 2.3.2. 独立构建下验证

在 TTS 组件目录下完成编译后，运行下列示例。

**C++ 简单合成（默认构建即包含）：**
```bash
cd /path/to/tts
mkdir -p build && cd build
cmake ..
make -j$(nproc)
./bin/tts_file_demo
./bin/tts_file_demo -p "你好世界" -l matcha:zh-en
```

**Python 文件合成：**
```bash
cd /path/to/tts
cmake --build build --target tts-install-python   # 或设置 PYTHONPATH
python python/examples/tts_file_demo.py
```

## 3. 应用开发

本章说明如何在自有工程中**集成 TTS 并调用 API**。环境与依赖见 [2.1](#21-安装依赖)，模型准备见 [2.2](#22-下载模型)，编译与运行示例见 [2.3](#23-测试)。

### 3.1. 构建与集成产物

无论通过 [2.3.1](#231-在-sdk-中验证)（SDK）或 [2.3.2](#232-独立构建下验证)（独立构建）哪种方式编译，完成后**应用开发所需**的库与头文件如下，集成时只需**包含头文件并链接对应库**：

| 产物 | 说明 |
| ---- | ---- |
| `include/tts_service.h` | **C++ API 头文件**，应用侧只需包含此头文件并链接下方库即可调用 |
| `build/lib/libtts.a` | C++ 核心库，链接时使用 |
| `build/python/spacemit_tts/` | Python 包，`cmake --build build --target tts-install-python` 安装后 `import spacemit_tts` |

示例可执行文件（非集成必需）：`build/bin/tts_file_demo`、`build/bin/tts_stream_demo`。运行与验证步骤见 [2.3.1](#231-在-sdk-中验证) 或 [2.3.2](#232-独立构建下验证)。

### 3.2. API 使用

**C++**：头文件 `include/tts_service.h` 为唯一 API 入口，实现为 PIMPL，无额外依赖。在业务代码中 `#include "tts_service.h"`，链接 `libtts.a`（及 ONNX Runtime、libsndfile 等），即可使用。

```cpp
#include "tts_service.h"
using namespace SpacemiT;

TtsConfig config = TtsConfig::Preset("matcha_zh_en");
config.speech_rate = 1.2f;
auto engine = std::make_shared<TtsEngine>(config);

auto result = engine->Call("你好世界");
if (result && result->IsSuccess()) result->SaveToFile("output.wav");

engine->StreamingCall("你好世界。今天天气很好。", callback);
```

**Python**：安装后 `import spacemit_tts`，详见 `python/examples/` 与 [API.md](API.md)。

```python
import spacemit_tts
spacemit_tts.synthesize_to_file("你好世界", "output.wav")
# 或
with spacemit_tts.Engine() as engine:
    result = engine.synthesize("你好世界")
    result.save("output.wav")
    print(result.duration_ms, result.rtf)
```

**CMake 集成**：将本组件作为子目录引入，并链接 `tts`、包含头文件路径即可。
```cmake
add_subdirectory(tts)
target_link_libraries(your_target PRIVATE tts)
target_include_directories(your_target PRIVATE ${TTS_SOURCE_DIR}/include)
```

## 4. 常见问题

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| 首次合成明显慢 | 模型加载和 warmup 开销 | 以 warmup 后的合成 RTF 评估性能。 |
| 英文发音异常 | `espeak-ng` 缺失或热词未配置 | 安装 `espeak-ng`，必要时用 `--lexicon` 指定读法。 |
| 播放速度或音调异常 | 播放采样率与模型采样率不一致 | 用 `audio_demo play` 或 `AudioPlayer` 播放时开启重采样。 |
| Kokoro 初始化很慢 | 模型和音色加载开销较大 | 应用启动阶段预初始化，不要在每轮对话里反复创建引擎。 |

## 5. 版本与发布

版本以本组件文档或仓库 tag 为准。

| 版本   | 说明 |
| ------ | ---- |
| 1.0.0  | 提供 C++ / Python 接口，支持 Matcha-TTS、Kokoro、文件/流式合成。 |

## 6. 贡献方式

欢迎参与贡献：提交 Issue 反馈问题，或通过 Pull Request 提交代码。

- **编码规范**：C++ 代码遵循 [Google C++ 风格指南](https://google.github.io/styleguide/cppguide.html)。
- **提交前检查**：若仓库提供 lint 脚本，请在提交前运行并通过检查。

## 7. License

本组件源码文件头声明为 Apache-2.0，最终以本目录 `LICENSE` 文件为准。

## 8. 附录：性能指标

以下数据基于 K3 平台（2 线程推理）实测，为阶段性信息，持续优化中，请以最新文档为准。

| 后端 | 测试文本 | 音频时长 | 处理时间 | RTF |
|------|----------|----------|----------|-----|
| MATCHA_ZH | "这是一个语音合成测试" | 2426ms | 1183ms | 0.49 |
| MATCHA_EN | "Hello, world" | 731ms | 440ms | 0.60 |
| MATCHA_ZH_EN | "今天学Python" | 1920ms | 715ms | 0.37 |
| KOKORO | "你好" | 2075ms | 13814ms | 6.66 |

测试命令：`./build/bin/tts_file_demo -p "<text>" -l <engine>`
