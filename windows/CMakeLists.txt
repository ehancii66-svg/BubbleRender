cmake_minimum_required(VERSION 3.16)
project(BubbleRender VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------- Dependencies (vcpkg) --------
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# -------- Source files --------
set(RENDER_SOURCES
    render/model.cpp
    render/shader.cpp
    render/camera.cpp
)

set(RENDER_HEADERS
    render/model.h
    render/mesh.h
    render/shader.h
    render/camera.h
    render/stb_image.h
    shader/shader_background.h
    shader/shader_quad.h
    shader/shader_refraction.h
    shader/shader_skybox.h
)

add_executable(${PROJECT_NAME}
    main.cpp
    ${RENDER_SOURCES}
    ${RENDER_HEADERS}
)

# -------- Include directories --------
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_CURRENT_SOURCE_DIR}/shader
)

# -------- Link libraries --------
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    glad::glad
    glm::glm
)

# -------- Copy assets to build directory --------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
)
