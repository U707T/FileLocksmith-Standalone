cmake_minimum_required(VERSION 3.15)
project(FileLocksmithStandalone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- 版本号
# 单一来源：仓库根目录的 VERSION 文件（ci.yml 也用同一个文件决定 tag）
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FL_VERSION)
string(STRIP "${FL_VERSION}" FL_VERSION)
string(REGEX MATCH "^([0-9]+)[.]([0-9]+)[.]([0-9]+)" FL_VERSION_MATCH "${FL_VERSION}")
if (NOT FL_VERSION_MATCH)
    message(FATAL_ERROR "VERSION 文件格式应为 x.y.z（可带 -rc 后缀），当前值: '${FL_VERSION}'")
endif()
set(FL_VERSION_MAJOR ${CMAKE_MATCH_1})
set(FL_VERSION_MINOR ${CMAKE_MATCH_2})
set(FL_VERSION_PATCH ${CMAKE_MATCH_3})
message(STATUS "FileLocksmith 版本: ${FL_VERSION} (${FL_VERSION_MAJOR}.${FL_VERSION_MINOR}.${FL_VERSION_PATCH})")
configure_file(res/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

add_executable(FileLocksmith WIN32
    src/main.cpp
    src/app_utils.cpp
    src/cli.cpp
    src/gui.cpp
    src/shell_integration.cpp
    src/file_locksmith.cpp
    src/ntdll_base.cpp
    src/ntdll_extensions.cpp
    res/resource.rc
)

target_compile_definitions(FileLocksmith PRIVATE UNICODE _UNICODE)
target_include_directories(FileLocksmith PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(FileLocksmith PRIVATE kernel32 user32 gdi32 shell32 advapi32 comctl32 psapi ole32 comdlg32)

if (MSVC)
    # Static CRT (/MT): the shipped exe must not need the VC++ redistributable.
    # The MinGW build is already static (-static), so both artifacts are
    # self-contained single files.
    set_property(TARGET FileLocksmith PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # /utf-8 is required: the UI strings in cli.cpp / gui.cpp are UTF-8.
    target_compile_options(FileLocksmith PRIVATE /utf-8 /W3 /permissive-)
    # Keep the Windows subsystem (no console flash on double click) but use the
    # plain "main" entry point so the same code can serve as a CLI tool.
    target_link_options(FileLocksmith PRIVATE /ENTRY:mainCRTStartup)
    # res/resource.rc already embeds the full application manifest (ComCtl32 v6,
    # DPI awareness, asInvoker) - the same file MinGW-w64 uses. Tell the linker
    # not to generate a second one, otherwise CVTRES fails with
    # "CVT1100: duplicate resource. type:MANIFEST, name:1".
    target_link_options(FileLocksmith PRIVATE /MANIFEST:NO)
else()
    target_compile_options(FileLocksmith PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_options(FileLocksmith PRIVATE -static -static-libgcc -static-libstdc++ -Wl,-e,mainCRTStartup)
endif()

set_target_properties(FileLocksmith PROPERTIES OUTPUT_NAME "FileLocksmith")
