cmake_minimum_required(VERSION 3.21)
project(RmskinTester VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(ZIP third_party/zlib)
add_library(minizip STATIC
    ${ZIP}/adler32.c ${ZIP}/crc32.c ${ZIP}/inflate.c ${ZIP}/inffast.c
    ${ZIP}/inftrees.c ${ZIP}/zutil.c
    ${ZIP}/minizip/ioapi.c ${ZIP}/minizip/iowin32.c ${ZIP}/minizip/unzip.c)
target_include_directories(minizip PUBLIC ${ZIP} ${ZIP}/minizip)
target_compile_definitions(minizip PRIVATE NOUNCRYPT _CRT_SECURE_NO_WARNINGS)

add_library(package STATIC src/Package.cpp src/CommandLine.cpp)
target_link_libraries(package PRIVATE minizip PUBLIC shell32 ole32 uuid advapi32 version shlwapi)
target_compile_definitions(package PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(rmskinTESTER WIN32 src/Main.cpp src/App.rc)
target_link_libraries(rmskinTESTER PRIVATE package comctl32 dwmapi gdiplus uxtheme)
target_compile_definitions(rmskinTESTER PRIVATE RMSKINTESTER_VERSION=L\"${PROJECT_VERSION}\")
add_executable(rmskinTESTER-cli src/ConsoleMain.cpp src/App.rc)
target_link_libraries(rmskinTESTER-cli PRIVATE package)
if(MSVC)
    foreach(target package rmskinTESTER rmskinTESTER-cli)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
    target_link_options(rmskinTESTER PRIVATE /MANIFEST:NO)
    target_link_options(rmskinTESTER-cli PRIVATE /MANIFEST:NO)
endif()
