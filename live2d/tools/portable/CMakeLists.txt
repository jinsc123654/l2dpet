# l2dpet-portable — 把 dist 文件夹打包成**一个**自解包 exe（Windows 单文件分发）
#
# 用法（scripts/windows_build.bat 会自己调，手工调也行）：
#   cmake -S live2d/tools/portable -B <build> \
#         -DPORTABLE_PAYLOAD_DIR=<dist\windows-x64> \
#         -DPORTABLE_APP_VERSION=<版本号> \
#         -DPORTABLE_OUT=<输出 exe 的完整路径>
#   cmake --build <build> --config Release
#
# 原理：把 payload 里每个文件作为一条 RCDATA 资源写进生成的 payload.rc，
# 启动器（portable.cpp）运行时按同一份清单（生成的 payload_files.h）写到
# %LOCALAPPDATA%\l2dpet\portable-<key>\ 再拉起 l2dtest.exe。不压缩、不依赖外部工具。
cmake_minimum_required(VERSION 3.14)
project(l2dpet_portable CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 与两个主程序一致：静态 CRT（/MT）——单文件 exe 也不依赖 VC++ 运行库
if(MSVC)
  foreach(_crt_var
          CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
          CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
          CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
          CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REGEX REPLACE "/MD" "/MT" ${_crt_var} "${${_crt_var}}")
  endforeach()
endif()

set(PORTABLE_PAYLOAD_DIR "" CACHE PATH "要打包的目录（dist\\windows-x64）")
set(PORTABLE_APP_VERSION "" CACHE STRING "版本号（只进解包目录名，变了就重解包）")
set(PORTABLE_ICON "" CACHE FILEPATH "exe 图标（可选，通常是 dist 里的 l2dpet.ico）")
set(PORTABLE_OUT "" CACHE FILEPATH "输出 exe 路径（默认放构建目录）")

if(NOT PORTABLE_PAYLOAD_DIR OR NOT IS_DIRECTORY "${PORTABLE_PAYLOAD_DIR}")
  message(FATAL_ERROR "PORTABLE_PAYLOAD_DIR 未设置或不是目录：${PORTABLE_PAYLOAD_DIR}")
endif()
if(NOT PORTABLE_APP_VERSION)
  set(PORTABLE_APP_VERSION "0")
endif()
# 版本号会进解包目录名：删掉字母数字点横线以外的字符（version.txt 里的 {a-1.0.0} 这种）
string(REGEX REPLACE "[^A-Za-z0-9._-]" "" PORTABLE_APP_VERSION "${PORTABLE_APP_VERSION}")
if(NOT PORTABLE_APP_VERSION)
  set(PORTABLE_APP_VERSION "0")
endif()

file(GLOB_RECURSE _payload RELATIVE "${PORTABLE_PAYLOAD_DIR}" "${PORTABLE_PAYLOAD_DIR}/*")
list(SORT _payload)

set(_hdr "// 本文件由 CMake 生成（live2d/tools/portable/CMakeLists.txt）：payload 清单\n#pragma once\n\nstruct PayloadFile\n{\n    int id;              // RCDATA 资源号\n    const wchar_t* rel;  // 相对解包目录（'/' 分隔）\n    unsigned long long size;\n};\n\nstatic const PayloadFile kPayloadFiles[] =\n{\n")
set(_rc "// 本文件由 CMake 生成：payload 里每个文件一条 RCDATA\n#pragma code_page(65001)\n#include <windows.h>\n\n")
set(_n 0)
set(_total 0)
set(_fingerprint "")

foreach(_f IN LISTS _payload)
  if(IS_DIRECTORY "${PORTABLE_PAYLOAD_DIR}/${_f}")
    continue()
  endif()
  math(EXPR _n "${_n} + 1")
  file(SIZE "${PORTABLE_PAYLOAD_DIR}/${_f}" _sz)
  file(SHA256 "${PORTABLE_PAYLOAD_DIR}/${_f}" _sha)
  string(APPEND _fingerprint "${_f}:${_sha}\n")
  math(EXPR _total "${_total} + ${_sz}")
  string(REPLACE "\\" "/" _src "${PORTABLE_PAYLOAD_DIR}/${_f}")
  string(APPEND _hdr "    { ${_n}, L\"${_f}\", ${_sz}ULL },\n")
  string(APPEND _rc "${_n} RCDATA \"${_src}\"\n")
endforeach()

if(_n EQUAL 0)
  message(FATAL_ERROR "payload 目录是空的：${PORTABLE_PAYLOAD_DIR}")
endif()

string(SHA256 _payload_hash "${_fingerprint}")
string(APPEND _hdr "};\n\nstatic const int kPayloadCount = ${_n};\n\n// 路径与内容的 SHA-256：同版本、同大小的修改也必须重新解包\nstatic const char kPayloadKey[] = \"${PORTABLE_APP_VERSION}-${_payload_hash}\";\n")

if(PORTABLE_ICON AND EXISTS "${PORTABLE_ICON}")
  string(REPLACE "\\" "/" _ico "${PORTABLE_ICON}")
  string(APPEND _rc "\n// 图标（exe 在资源管理器/任务栏/Alt-Tab 都取它）\n1 ICON \"${_ico}\"\n")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/payload_files.h" "${_hdr}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/payload.rc" "${_rc}")
message(STATUS "便携包 payload：${_n} 个文件，共 ${_total} 字节（key=${PORTABLE_APP_VERSION}-${_payload_hash}）")

add_executable(l2dpet_portable WIN32
  "${CMAKE_CURRENT_SOURCE_DIR}/portable.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/payload.rc")
target_include_directories(l2dpet_portable PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(l2dpet_portable PRIVATE UNICODE _UNICODE)
target_link_libraries(l2dpet_portable PRIVATE shell32 user32 ole32)
set_target_properties(l2dpet_portable PROPERTIES OUTPUT_NAME "l2dpet-portable")

if(MSVC)
  # 源码/生成的清单都是 UTF-8（中文注释与提示词）：不写 /utf-8 会被按 936 解释而报错
  target_compile_options(l2dpet_portable PRIVATE /utf-8)
  # 资源里塞着十几 MB 的 payload：别让增量链接拖时间
  target_link_options(l2dpet_portable PRIVATE /INCREMENTAL:NO)
endif()

if(PORTABLE_OUT)
  add_custom_command(TARGET l2dpet_portable POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:l2dpet_portable> "${PORTABLE_OUT}"
    COMMENT "便携单文件 → ${PORTABLE_OUT}")
endif()
