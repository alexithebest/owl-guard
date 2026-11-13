cmake_minimum_required(VERSION 3.20)
project(OwlGuard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(OwlGuard WIN32
    src/main.cpp
    src/guard.cpp
    src/overlay.cpp
)

target_compile_definitions(OwlGuard PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

target_compile_options(OwlGuard PRIVATE /utf-8 /W4 /permissive-)

target_link_libraries(OwlGuard PRIVATE
    user32 gdi32 dwmapi shell32 ole32 advapi32 ntdll
    wintrust crypt32 rstrtmgr
)

# manifest: per-monitor DPI, asInvoker (o driver pede elevacao, o watchdog nao)
target_sources(OwlGuard PRIVATE src/app.manifest)
