cmake_minimum_required(VERSION 3.15)
project(DetourFlow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 1. 编译 Microsoft Detours 静态库
add_library(detours STATIC
    detours/src/creatwth.cpp
    detours/src/detours.cpp
    detours/src/disasm.cpp
    detours/src/image.cpp
    detours/src/modules.cpp
)
target_include_directories(detours PUBLIC detours/src)

# 2. 编译 Hook DLL: DetourFlow
add_library(DetourFlow SHARED
    DetourFlow.cpp
)
target_link_libraries(DetourFlow PRIVATE detours ws2_32)

# 3. 编译启动器: DetourLauncher
add_executable(DetourLauncher
    DetourLauncher.cpp
)
target_link_libraries(DetourLauncher PRIVATE detours)

# Change output directory to avoid locking issues with active DLL
set_target_properties(DetourFlow PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release_new"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release_new"
)
set_target_properties(DetourLauncher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release_new"
)

