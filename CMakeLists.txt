cmake_minimum_required(VERSION 3.16)
project(tts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Position Independent Code for all targets (required for shared libs/Python bindings)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 可执行文件输出到 build/bin 目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# =============================================================================
# 第三方依赖 - 通过 cmake/ 模块自动获取到 ~/.cache/thirdparty
# =============================================================================
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cppjieba.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpp_pinyin.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/kaldifst.cmake)

# =============================================================================
# TTS Library
# =============================================================================

# 源文件
set(TTS_SOURCES
    src/tts_engine.cpp
    src/tts_backend_factory.cpp
    src/audio/audio_processor.cpp
    src/text/text_utils.cpp
    src/text/number_utils.cpp
    src/text/fst_normalizer.cpp
    src/text/text_normalizer.cpp
    src/text/token_utils.cpp
    src/text/phoneme_utils.cpp
    src/vocoder/vocoder.cpp
    src/backends/matcha/matcha_backend.cpp
    src/backends/matcha/matcha_zh_backend.cpp
    src/backends/matcha/matcha_en_backend.cpp
    src/backends/matcha/matcha_zh_en_backend.cpp
    src/backends/matcha/tts_model_downloader.cpp
    src/backends/kokoro/kokoro_backend.cpp
    src/backends/kokoro/kokoro_phonemizer.cpp
    src/backends/kokoro/kokoro_voice_manager.cpp
    src/backends/kokoro/kokoro_model_downloader.cpp
    src/tts_presets.cpp
)

# cpp-pinyin 源文件
set(CPP_PINYIN_SRC_DIR ${CPP_PINYIN_DIR}/src)
list(APPEND TTS_SOURCES
    ${CPP_PINYIN_SRC_DIR}/ChineseG2p.cpp
    ${CPP_PINYIN_SRC_DIR}/DictUtil.cpp
    ${CPP_PINYIN_SRC_DIR}/G2pglobal.cpp
    ${CPP_PINYIN_SRC_DIR}/Pinyin.cpp
    ${CPP_PINYIN_SRC_DIR}/PinyinRes.cpp
    ${CPP_PINYIN_SRC_DIR}/ToneConverter.cpp
    ${CPP_PINYIN_SRC_DIR}/U16Str.cpp
    ${CPP_PINYIN_SRC_DIR}/toneUtil/ManTone.cpp
    ${CPP_PINYIN_SRC_DIR}/toneUtil/ManToneUtil.cpp
)

# 创建静态库
add_library(tts STATIC ${TTS_SOURCES})

# Include 目录
target_include_directories(tts
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# =============================================================================
# 依赖项
# =============================================================================

# ONNX Runtime
# 用户可通过以下变量自定义路径:
#   -DONNXRUNTIME_INCLUDE_DIR=/path/to/include
#   -DONNXRUNTIME_LIB="/path/to/libonnxruntime.so;/path/to/other.so"
#
# 示例 (RISC-V):
#   cmake -B build -S . \
#     -DONNXRUNTIME_INCLUDE_DIR=$HOME/codes/spacemit-ort.riscv64.2.0.2+beta0/include \
#     -DONNXRUNTIME_LIB="$HOME/codes/spacemit-ort.riscv64.2.0.2+beta0/lib/libonnxruntime.so;$HOME/muggle/lib/libspacemit_ep.so"

set(ONNXRUNTIME_INCLUDE_DIR "" CACHE PATH "ONNX Runtime include directory")
set(ONNXRUNTIME_LIB "" CACHE STRING "ONNX Runtime libraries (semicolon-separated)")

if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
    # 使用用户指定的路径
    message(STATUS "[tts] Using user-specified ONNX Runtime:")
    message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_DIR}")
    message(STATUS "  Lib: ${ONNXRUNTIME_LIB}")
    target_include_directories(tts PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(tts PUBLIC ${ONNXRUNTIME_LIB})
else()
    # 先尝试直接查找（避免损坏的 CMake 配置文件问题）
    if(APPLE)
        # macOS Homebrew
        find_library(ONNXRUNTIME_LIB_AUTO onnxruntime HINTS /opt/homebrew/lib /usr/local/lib)
        find_path(ONNXRUNTIME_INCLUDE_AUTO onnxruntime_cxx_api.h HINTS /opt/homebrew/include /usr/local/include)
    else()
        # Linux 默认路径
        find_library(ONNXRUNTIME_LIB_AUTO onnxruntime HINTS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
        find_path(ONNXRUNTIME_INCLUDE_AUTO onnxruntime_cxx_api.h HINTS /usr/include /usr/local/include /usr/include/onnxruntime)
    endif()

    if(ONNXRUNTIME_LIB_AUTO AND ONNXRUNTIME_INCLUDE_AUTO)
        message(STATUS "[tts] Found ONNX Runtime via direct search:")
        message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_AUTO}")
        message(STATUS "  Lib: ${ONNXRUNTIME_LIB_AUTO}")
        target_link_libraries(tts PUBLIC ${ONNXRUNTIME_LIB_AUTO})
        target_include_directories(tts PUBLIC ${ONNXRUNTIME_INCLUDE_AUTO})
    else()
        # 直接查找失败，尝试 find_package 作为备选
        find_package(onnxruntime QUIET)
        if(onnxruntime_FOUND)
            message(STATUS "[tts] Found ONNX Runtime via find_package")
            target_link_libraries(tts PUBLIC onnxruntime::onnxruntime)
        else()
            message(WARNING "[tts] ONNX Runtime not found! Please specify:")
            message(WARNING "  -DONNXRUNTIME_INCLUDE_DIR=/path/to/include")
            message(WARNING "  -DONNXRUNTIME_LIB=/path/to/libonnxruntime.so")
        endif()
    endif()
endif()

# FFTW3 (for audio processing)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3 QUIET fftw3)
    pkg_check_modules(FFTW3F QUIET fftw3f)
endif()

if(FFTW3_FOUND)
    target_link_directories(tts PUBLIC ${FFTW3_LIBRARY_DIRS})
    target_link_libraries(tts PUBLIC ${FFTW3_LIBRARIES})
    target_include_directories(tts PUBLIC ${FFTW3_INCLUDE_DIRS})
endif()

# FFTW3F (float version - required for vocoder)
if(FFTW3F_FOUND)
    target_link_directories(tts PUBLIC ${FFTW3F_LIBRARY_DIRS})
    target_link_libraries(tts PUBLIC ${FFTW3F_LIBRARIES})
else()
    # macOS Homebrew fallback for fftw3f
    if(APPLE)
        find_library(FFTW3F_LIB fftw3f HINTS
            /opt/homebrew/opt/fftw/lib
            /opt/homebrew/lib
            /usr/local/lib)
        if(FFTW3F_LIB)
            target_link_libraries(tts PUBLIC ${FFTW3F_LIB})
        else()
            message(WARNING "FFTW3F not found")
        endif()
    endif()
endif()

if(NOT FFTW3_FOUND AND NOT FFTW3F_FOUND)
    # macOS Homebrew fallback
    if(APPLE)
        find_library(FFTW3_LIB fftw3 HINTS
            /opt/homebrew/opt/fftw/lib
            /opt/homebrew/lib
            /usr/local/lib)
        find_path(FFTW3_INCLUDE fftw3.h HINTS
            /opt/homebrew/opt/fftw/include
            /opt/homebrew/include
            /usr/local/include)
        if(FFTW3_LIB AND FFTW3_INCLUDE)
            target_link_libraries(tts PUBLIC ${FFTW3_LIB})
            target_include_directories(tts PUBLIC ${FFTW3_INCLUDE})
        else()
            message(WARNING "FFTW3 not found")
        endif()
    endif()
endif()

# libcurl (for model download)
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(tts PRIVATE CURL::libcurl)
endif()

# =============================================================================
# 第三方库 Include 目录
# =============================================================================

# cppjieba include
target_include_directories(tts PRIVATE
    ${CPPJIEBA_DIR}/include
    ${CPPJIEBA_DIR}/deps/limonp/include
)

# kaldifst (FST 文本规范化)
if(TTS_USE_FST)
    target_include_directories(tts PRIVATE ${KALDIFST_INCLUDE_DIRS})
    target_link_libraries(tts PRIVATE kaldifst_core)
    target_compile_definitions(tts PRIVATE TTS_USE_FST)
endif()

# cpp-pinyin include
target_include_directories(tts PRIVATE
    ${CPP_PINYIN_DIR}/include
    ${CPP_PINYIN_DIR}/src
    ${CPP_PINYIN_DIR}/src/toneUtil
)

# espeak-ng (英文音素)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ESPEAK QUIET espeak-ng)
    if(ESPEAK_FOUND)
        target_link_directories(tts PUBLIC ${ESPEAK_LIBRARY_DIRS})
        target_link_libraries(tts PUBLIC ${ESPEAK_LIBRARIES})
        target_include_directories(tts PRIVATE ${ESPEAK_INCLUDE_DIRS})
    else()
        # macOS Homebrew fallback
        if(APPLE)
            find_library(ESPEAK_LIB espeak-ng HINTS
                /opt/homebrew/opt/espeak-ng/lib
                /opt/homebrew/Cellar/espeak-ng/1.52.0/lib
                /opt/homebrew/lib
                /usr/local/lib)
            find_path(ESPEAK_INCLUDE espeak-ng/speak_lib.h HINTS
                /opt/homebrew/opt/espeak-ng/include
                /opt/homebrew/Cellar/espeak-ng/1.52.0/include
                /opt/homebrew/include
                /usr/local/include)
            if(ESPEAK_LIB AND ESPEAK_INCLUDE)
                target_link_libraries(tts PUBLIC ${ESPEAK_LIB})
                target_include_directories(tts PRIVATE ${ESPEAK_INCLUDE})
            else()
                message(WARNING "espeak-ng not found")
            endif()
        endif()
    endif()
endif()

# =============================================================================
# 导出
# =============================================================================

# 安装头文件
install(DIRECTORY include/ DESTINATION include/tts)

# 安装库
install(TARGETS tts
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# kaldifst 通过 add_subdirectory(... EXCLUDE_FROM_ALL) 引入, 其自带的 install 规则被屏蔽。
# 由于 libtts.a (fst_normalizer.cpp.o) PUBLIC-需要 kaldifst_core 的符号, 下游消费者
# (如 application/native/omni_agent/voice_chat) 必须能链到 kaldifst_core + kaldifst_fst。
# OpenFst 只开启了 HAVE_SCRIPT, 产物仅一个 libkaldifst_fst.a (OUTPUT_NAME 被重命名过)。
if(TTS_USE_FST AND TARGET kaldifst_core AND TARGET fst)
    install(TARGETS kaldifst_core fst ARCHIVE DESTINATION lib)
endif()

# =============================================================================
# Demo 可执行程序
# =============================================================================
# BUILD_STREAM_DEMO=ON 时编译流式合成示例，需 PortAudio 与 audio 组件。
# 用法: cmake .. -DBUILD_STREAM_DEMO=ON
#
option(BUILD_SIMPLE_DEMO "Build TTS file/simple demo" ON)

# 检测是否在 SDK build 流程内编译（envsetup.sh 已 source 则 SROBOTIS_OUTPUT_ROOT 被 export）。
# 仅文件存在不算——源码可能在仓库里，但用户裸 cmake 时 audio 组件并不一定已装，
# 此时不应默认打开 BUILD_STREAM_DEMO。
if(DEFINED ENV{SROBOTIS_OUTPUT_ROOT} AND NOT "$ENV{SROBOTIS_OUTPUT_ROOT}" STREQUAL "")
    set(_IN_SDK_BUILD ON)
else()
    set(_IN_SDK_BUILD OFF)
endif()

option(BUILD_STREAM_DEMO "Build TTS stream demo (requires audio component + PortAudio)" ${_IN_SDK_BUILD})

if(BUILD_SIMPLE_DEMO)
    add_executable(tts_file_demo examples/tts_file_demo.cpp)
    target_link_libraries(tts_file_demo PRIVATE tts)
    set_target_properties(tts_file_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    install(TARGETS tts_file_demo RUNTIME DESTINATION bin)
endif()

# 流式 TTS 演示程序 (需要 space_audio / portaudio)
if(BUILD_STREAM_DEMO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
    pkg_check_modules(SAMPLERATE samplerate)

    find_library(SPACE_AUDIO_LIB space_audio)
    find_library(AUDIO_RESAMPLER_LIB audio_resampler)
    find_path(SPACE_AUDIO_INC audio_base.hpp)

    if(NOT SPACE_AUDIO_LIB OR NOT AUDIO_RESAMPLER_LIB OR NOT SPACE_AUDIO_INC)
        message(FATAL_ERROR
            "BUILD_STREAM_DEMO=ON but audio component not found!\n"
            "Install audio component first, or disable with -DBUILD_STREAM_DEMO=OFF")
    endif()

    find_package(Threads REQUIRED)

    add_executable(tts_stream_demo examples/tts_stream_demo.cpp)

    target_include_directories(tts_stream_demo PRIVATE
        ${SPACE_AUDIO_INC}
        ${PORTAUDIO_INCLUDE_DIRS}
    )

    target_link_libraries(tts_stream_demo PRIVATE
        tts
        ${SPACE_AUDIO_LIB}
        ${AUDIO_RESAMPLER_LIB}
        ${PORTAUDIO_LIBRARIES}
        ${SAMPLERATE_LIBRARIES}
        Threads::Threads
    )
    target_link_directories(tts_stream_demo PRIVATE
        ${PORTAUDIO_LIBRARY_DIRS}
        ${SAMPLERATE_LIBRARY_DIRS}
    )

    if(SAMPLERATE_FOUND)
        target_compile_definitions(tts_stream_demo PRIVATE USE_LIBSAMPLERATE)
    endif()

    set_target_properties(tts_stream_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    install(TARGETS tts_stream_demo RUNTIME DESTINATION bin)

    message(STATUS "[tts] tts_stream_demo enabled")
else()
    message(STATUS "[tts] tts_stream_demo disabled (BUILD_STREAM_DEMO=OFF)")
endif()

# =============================================================================
# Python Bindings (optional)
# =============================================================================

option(BUILD_PYTHON_BINDINGS "Build Python bindings" ON)

if(BUILD_PYTHON_BINDINGS)
    # 1. Find Python interpreter and development libraries
    find_package(Python3 COMPONENTS Interpreter Development QUIET)

    if(Python3_FOUND)
        message(STATUS "[tts] Python found: ${Python3_EXECUTABLE} (${Python3_VERSION})")
    else()
        message(STATUS "[tts] Python bindings disabled (Python3 not found)")
    endif()

    # 2. Find pybind11 (try system first, then Python module)
    find_package(pybind11 CONFIG QUIET)

    if(NOT pybind11_FOUND AND Python3_FOUND)
        # Try to get pybind11 from Python module
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import pybind11; print(pybind11.get_cmake_dir())"
            OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE PYBIND11_RESULT
        )
        if(PYBIND11_RESULT EQUAL 0)
            list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
            find_package(pybind11 CONFIG QUIET)
        endif()
    endif()

    # 3. Build Python bindings module
    if(pybind11_FOUND)
        pybind11_add_module(_spacemit_tts ${CMAKE_CURRENT_SOURCE_DIR}/python/tts_bindings.cpp)

        target_include_directories(_spacemit_tts PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

        target_link_libraries(_spacemit_tts PRIVATE tts)

        set_target_properties(_spacemit_tts PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python
        )

        message(STATUS "[tts] Python bindings enabled (_spacemit_tts module for Python ${Python3_VERSION})")

        # 4. Get Python site-packages path for installation
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import sysconfig; print(sysconfig.get_path('purelib'))"
            OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        # 5. Custom install target: make tts-install-python
        add_custom_target(tts-install-python
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${PYTHON_SITE_PACKAGES}/spacemit_tts
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/python/spacemit_tts
                ${PYTHON_SITE_PACKAGES}/spacemit_tts
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:_spacemit_tts>
                ${PYTHON_SITE_PACKAGES}/spacemit_tts/
            DEPENDS _spacemit_tts
            COMMENT "Installing spacemit_tts to ${PYTHON_SITE_PACKAGES}/spacemit_tts/"
        )

        # ---- wheel 自动构建与 install（对接 SDK 顶层产物）----
        set(TTS_WHEEL_BUILD_ROOT      "${CMAKE_BINARY_DIR}/python_wheel")
        set(TTS_WHEEL_OUTPUT_DIR      "${TTS_WHEEL_BUILD_ROOT}/dist")
        set(TTS_WHEEL_CMAKE_BUILD_DIR "${TTS_WHEEL_BUILD_ROOT}/cmake")
        set(TTS_WHEEL_SOURCE_DIR      "${TTS_WHEEL_BUILD_ROOT}/src")
        set(TTS_WHEEL_STAMP           "${TTS_WHEEL_BUILD_ROOT}/wheel.stamp")

        if(DEFINED ENV{SROBOTIS_OUTPUT_ROOT} AND NOT "$ENV{SROBOTIS_OUTPUT_ROOT}" STREQUAL "")
            set(TTS_DIST_INSTALL_DIR "$ENV{SROBOTIS_OUTPUT_ROOT}/dist")
        else()
            set(TTS_DIST_INSTALL_DIR "${CMAKE_BINARY_DIR}/dist")
        endif()

        file(MAKE_DIRECTORY "${TTS_WHEEL_OUTPUT_DIR}")

        add_custom_command(
            OUTPUT  ${TTS_WHEEL_STAMP}
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${TTS_WHEEL_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${TTS_WHEEL_SOURCE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TTS_WHEEL_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TTS_WHEEL_CMAKE_BUILD_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/python ${TTS_WHEEL_SOURCE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${TTS_WHEEL_SOURCE_DIR}/README.md
            COMMAND ${CMAKE_COMMAND} -E chdir ${TTS_WHEEL_SOURCE_DIR}
                    ${CMAKE_COMMAND} -E env
                    SPACEMIT_PREBUILT_EXTENSION=$<TARGET_FILE:_spacemit_tts>
                    SPACEMIT_CMAKE_BUILD_DIR=${TTS_WHEEL_CMAKE_BUILD_DIR}
                    ${Python3_EXECUTABLE} -m pip wheel . --no-deps --wheel-dir ${TTS_WHEEL_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${TTS_WHEEL_STAMP}
            DEPENDS
                _spacemit_tts
                ${CMAKE_CURRENT_SOURCE_DIR}/README.md
                ${CMAKE_CURRENT_SOURCE_DIR}/python/setup.py
                ${CMAKE_CURRENT_SOURCE_DIR}/python/pyproject.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/python/spacemit_tts/__init__.py
            VERBATIM
            COMMENT "Building spacemit_tts wheel into ${TTS_WHEEL_OUTPUT_DIR}"
        )

        add_custom_target(tts-python-wheel ALL DEPENDS ${TTS_WHEEL_STAMP})

        install(
            DIRECTORY ${TTS_WHEEL_OUTPUT_DIR}/
            DESTINATION ${TTS_DIST_INSTALL_DIR}
            FILES_MATCHING PATTERN "*.whl"
        )
    else()
        message(STATUS "[tts] Python bindings disabled (pybind11 not found)")
        message(STATUS "  Install pybind11: pip install pybind11")
    endif()
endif()

# =============================================================================
# Summary
# =============================================================================
message(STATUS "")
message(STATUS "=== TTS Build Configuration ===")
message(STATUS "  BUILD_SIMPLE_DEMO:      ${BUILD_SIMPLE_DEMO}")
message(STATUS "  BUILD_STREAM_DEMO:      ${BUILD_STREAM_DEMO}")
message(STATUS "  BUILD_PYTHON_BINDINGS:  ${BUILD_PYTHON_BINDINGS}")
if(BUILD_PYTHON_BINDINGS AND pybind11_FOUND)
    message(STATUS "  Wheel output:           ${TTS_WHEEL_OUTPUT_DIR}")
    message(STATUS "  Wheel install dir:      ${TTS_DIST_INSTALL_DIR}")
endif()
message(STATUS "")
