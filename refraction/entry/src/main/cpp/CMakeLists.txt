# the minimum version of CMake.
cmake_minimum_required(VERSION 3.5.0)
project(Refraction)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
#set(ASSIMP_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include
                    ${NATIVERENDER_ROOT_PATH}/thirdParty
                    ${NATIVERENDER_ROOT_PATH}/thirdParty/assimp
)

add_library(entry SHARED
    napi_init.cpp
    render/model.cpp
    render/shader.cpp
    render/camera.cpp
)

if (OHOS_ARCH STREQUAL "arm64-v8a")
    set(ASSIMP_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/libs/arm64-v8a/libassimp.so.5)
elseif (OHOS_ARCH STREQUAL "x86_64")
    set(ASSIMP_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/libs/x86_64/libassimp.so.5)
else()
    message(FATAL_ERROR "Unsupported architecture: ${OHOS_ARCH}")
endif()

add_library(libassimp-shared SHARED IMPORTED)
set_target_properties(
    libassimp-shared
    PROPERTIES
    IMPORTED_LOCATION
    ${ASSIMP_LIB_PATH}
)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    librawfile.z.so
    libnative_window.so
    libnative_buffer.so
    libGLESv3.so
    libEGL.so
    libassimp-shared
)