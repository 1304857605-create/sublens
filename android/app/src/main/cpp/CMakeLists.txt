# ---------------------------------------------------------------------------
# SubLens · Android NDK 构建
#
# 与桌面构建共用同一份核心源码(core 是纯 C++20、零依赖的), 这里只是把它
# 编成 .so 再加一层 JNI 桥(engine_jni.cpp):
#   libsublens.so = 核心引擎 + JNI 桥 + AAudio 采集适配
#
# 由 android/app/build.gradle.kts 的 externalNativeBuild 调用。
# 前置: Android NDK r25+ (完整 C++20), minSdk 26 (AAudio), targetSdk 34
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)
project(sublens_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 复用桌面端的核心源码(只排除桌面演示程序的 main)
file(GLOB_RECURSE SUBLENS_CORE_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/../../../../../src/*.cpp")
list(FILTER SUBLENS_CORE_SRC EXCLUDE REGEX "/src/app/main\\.cpp$")

file(GLOB SUBLENS_ANDROID_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/*.cpp")

add_library(sublens SHARED ${SUBLENS_CORE_SRC} ${SUBLENS_ANDROID_SRC})

target_include_directories(sublens PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../../../../include")

target_link_libraries(sublens PRIVATE
  android   # ANativeWindow / 系统能力
  aaudio    # 低延迟音频采集 (Android 8.0+)
  log)      # __android_log_print

# 真机调试: adb logcat -s SubLens
target_compile_definitions(sublens PRIVATE SUBLENS_ANDROID=1)
target_compile_options(sublens PRIVATE -Wall -Wextra -fvisibility=hidden)
