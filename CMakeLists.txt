cmake_minimum_required(VERSION 3.20)
project(SubLens VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# SubLens · 智能眼镜超低延迟实时字幕翻译引擎
#
# 设计原则:
#   1) 核心引擎 (sublens_core) 是纯 C++20、零第三方依赖, 因此在桌面(Windows/Linux)
#      和 Android NDK 上都能原样编译 —— 这是"同一份代码跑在眼镜上"的前提。
#   2) 线程模型固定, 热路径上不做堆分配、不拿锁(见 docs/ARCHITECTURE.md)。
#   3) 桌面端可以直接跑终端 HUD 演示与基准测试, 不需要真机。
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# --- 核心库: 用 GLOB 收集源文件, 新增 .cpp 不需要改本文件 -------------------
file(GLOB_RECURSE SUBLENS_CORE_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER SUBLENS_CORE_SRC EXCLUDE REGEX "/src/app/main\\.cpp$")

add_library(sublens_core STATIC ${SUBLENS_CORE_SRC})
target_include_directories(sublens_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sublens_core PUBLIC Threads::Threads)

if(NOT MSVC)
  target_compile_options(sublens_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# --- 演示程序 ---------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
  add_executable(sublens_demo "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
  target_link_libraries(sublens_demo PRIVATE sublens_core)
endif()

# --- 单元测试 (各测试文件只注册用例, main 在 tests/tiny_test_main.cpp) ------
file(GLOB SUBLENS_TEST_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
if(SUBLENS_TEST_SRC)
  add_executable(sublens_tests ${SUBLENS_TEST_SRC})
  target_link_libraries(sublens_tests PRIVATE sublens_core)
  enable_testing()
  add_test(NAME sublens_tests COMMAND sublens_tests)
endif()

# --- 基准测试 ---------------------------------------------------------------
file(GLOB SUBLENS_BENCH_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")
if(SUBLENS_BENCH_SRC)
  add_executable(sublens_bench ${SUBLENS_BENCH_SRC})
  target_link_libraries(sublens_bench PRIVATE sublens_core)
endif()
