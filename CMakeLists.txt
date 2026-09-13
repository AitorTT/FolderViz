cmake_minimum_required(VERSION 3.20)
project(FolderViz CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(FolderViz WIN32
    src/main.cpp
    src/app.cpp
    src/scanner.cpp
    src/treemap.cpp
    src/city.cpp
    src/filetype.cpp
    app.manifest
)

target_compile_definitions(FolderViz PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(FolderViz PRIVATE /W4 /permissive- /utf-8)

target_link_libraries(FolderViz PRIVATE
    comctl32
    gdi32
    gdiplus
    user32
    shell32
    ole32
    uuid
    shlwapi
)
