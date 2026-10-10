# l2dpet-ui — 桌宠的全部 UI：托盘 / 右键菜单 / 说话气泡 / 状态与设置（EUI-NEO：C++17，GLFW+OpenGL，Linux/Windows/macOS）
#
# 它只是桌宠协议（PROTOCOL-LLM.md）的一个客户端：连 127.0.0.1:<端口> 轮询 panel_state、发命令，
# 和 LLM agent 走同一条路；桌宠进程本身不依赖它（没有它也能跑）。
#
# EUI-NEO 来源（二选一）：
#   -DEUI_NEO_DIR=<本地源码>（默认 ~/l2d-build/3rd/EUI-NEO，存在就用，可离线）
#   否则 FetchContent 拉固定提交（需要网络）
cmake_minimum_required(VERSION 3.14)
project(l2dpet_panel LANGUAGES C CXX)

# ---- Windows：静态 CRT（/MT）----
# 与 player 侧同样的理由：产物不依赖 MSVCP140/VCRUNTIME140（干净的 Windows 双击就能跑）。
# 直接改编译 flag 而不是 CMAKE_MSVC_RUNTIME_LIBRARY：EUI-NEO 里的 glfw/freetype 各自的
# cmake_minimum_required 会把该策略在它们目录里重置回旧行为，混用 /MD、/MT 会 LNK2038。
if(MSVC)
  foreach(_crt_var
          CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
          CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
          CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
          CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REGEX REPLACE "/MD" "/MT" ${_crt_var} "${${_crt_var}}")
  endforeach()
  set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)   # EUI 里 glfw 的开关（默认 DLL 版）
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EUI-NEO 已作为**本地源码**随仓库分发（live2d/3rd/EUI-NEO），不再依赖 git/网络：
# 三端（Linux / Windows / macOS）都能直接编，工程需要的改动已落在那些文件里。
set(EUI_NEO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/EUI-NEO" CACHE PATH "EUI-NEO 源码目录（默认用仓库内自带的本地副本）")
# 本地副本对应的上游提交（留作对照；eui-patches/ 是相对它的改动记录）
set(EUI_NEO_COMMIT 782c56993dc1890e0589e2100cfa74322bb0e0bf)   # 2026-09-22 main

# 只要库本身：不编它的示例/应用/可选模块
set(EUI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_USER_APPS OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_TRAY ON CACHE BOOL "" FORCE)    # 托盘也归 EUI（桌宠进程不再有任何 UI 控件）

# exe 图标用本工程自己的（EUI 会把它写成生成的 .rc 挂到目标上；
# 别再自己另加一个 .rc —— 会跟它撞成"资源重复 ICON" 链接失败）
if(WIN32)
  set(EUI_APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/l2dpet.ico")
endif()

# 本工程对 EUI-NEO 的补丁（弹出窗口/窗口控制/onTick/托盘自定义菜单），见 eui-patches/README.md
file(GLOB EUI_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/eui-patches/*.patch")
list(SORT EUI_PATCHES)
function(eui_apply_patches dir)
  # 幂等做法：每次配置都先把工作区还原到 HEAD，再按序重打一遍所有补丁。
  #
  # 不能靠"反向 check 能过就当成已应用、跳过"来判断（那是上游原来的写法）：
  # 补丁之间会改**同一个文件**（0002 就落在 0001 改过的 core/app/glfw_app_main.cpp 上），
  # 于是第二次配置时反向 check 的上下文对不上 → 跳不过；正向再打一次 → 又因为已经打过而失败
  # → 报"补丁打不上（EUI 版本不是 …？）"，明明版本是对的。整片还原再按序打才是真幂等。
  execute_process(COMMAND git -C "${dir}" checkout -- . RESULT_VARIABLE r0 OUTPUT_QUIET ERROR_QUIET)
  if(NOT r0 EQUAL 0)
    message(WARNING "EUI-NEO 工作区还原失败（git checkout -- .）：${dir}——不是 git 检出？补丁可能打不上。")
  endif()
  foreach(p ${EUI_PATCHES})
    execute_process(COMMAND git -C "${dir}" apply "${p}" RESULT_VARIABLE r ERROR_QUIET)
    if(NOT r EQUAL 0)
      message(FATAL_ERROR "EUI-NEO 补丁打不上（EUI 版本不是 ${EUI_NEO_COMMIT}？）：${p}")
    endif()
    message(STATUS "EUI-NEO 补丁已应用：${p}")
  endforeach()
endfunction()

if(NOT EXISTS "${EUI_NEO_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "找不到 EUI-NEO 源码：${EUI_NEO_DIR}\n"
    "仓库自带的本地副本在 live2d/3rd/EUI-NEO（正常不该缺）。删过它、或用外部检出时，\n"
    "用 -DEUI_NEO_DIR=<目录> 指定即可（外部检出需落在提交 ${EUI_NEO_COMMIT} 上）。")
endif()

# 仓库自带的那份本地副本**已经含**了 eui-patches/ 的改动，不用再打补丁；
# 只有指向外部检出（应当落在钉死的提交上）时才按序补一下。
set(EUI_NEO_VENDORED "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/EUI-NEO")
# 比较要归一化：调用方可能用反斜杠（windows_build.bat 传的就是）或相对路径，
# 直接 STREQUAL 会判成"不同"——于是把仓库内那份当成外部检出又打一遍补丁（它本来就已经含补丁）。
get_filename_component(_eui_d "${EUI_NEO_DIR}" ABSOLUTE)
get_filename_component(_eui_v "${EUI_NEO_VENDORED}" ABSOLUTE)
if(_eui_d STREQUAL _eui_v)
  message(STATUS "EUI-NEO：仓库自带本地源码（已含 eui-patches/ 的改动，不再打补丁）")
else()
  message(STATUS "EUI-NEO：外部源码 ${EUI_NEO_DIR} —— 按序应用 eui-patches/")
  eui_apply_patches("${EUI_NEO_DIR}")
endif()

add_subdirectory("${EUI_NEO_DIR}" eui-neo)

set(L2D_PANEL_SOURCES app.cpp petlink.cpp i18n.cpp)

# 程序版本（仓库根 version.txt，形如 {a-1.0.0}）：关于页要显示。文件不在就给 dev。
set(L2D_VERSION "dev")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../version.txt")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../version.txt" _l2d_ver)
  string(STRIP "${_l2d_ver}" _l2d_ver)
  string(REPLACE "{" "" _l2d_ver "${_l2d_ver}")   # version.txt 里是 {a-1.0.0} 这种写法，显示去掉花括号
  string(REPLACE "}" "" _l2d_ver "${_l2d_ver}")
  string(STRIP "${_l2d_ver}" _l2d_ver)
  if(_l2d_ver)
    set(L2D_VERSION "${_l2d_ver}")
  endif()
endif()
message(STATUS "l2dpet 版本：${L2D_VERSION}")
# exe 图标由 EUI 的 eui_neo_configure_app() 按 EUI_APP_ICON_ICO（上面设成 l2dpet.ico）生成资源挂上，
# 这里不再自己加 .rc —— 会有两个 ICON 资源，链接报 CVT1100/LNK1123。
add_executable(l2dpet-ui ${L2D_PANEL_SOURCES})
target_include_directories(l2dpet-ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../player/src)
target_compile_definitions(l2dpet-ui PRIVATE L2D_VERSION="${L2D_VERSION}")   # 关于页显示的版本
if(WIN32)
  # comdlg32/shell32/ole32：原生文件与目录选择对话框（GetOpenFileNameW / SHBrowseForFolderW）
  target_link_libraries(l2dpet-ui PRIVATE ws2_32 comdlg32 shell32 ole32)
endif()
eui_neo_configure_app(l2dpet-ui)
# 托盘图标：放在程序旁边（桌宠启动 UI 进程时工作目录 = 程序目录）。
# 注意托盘要的是 **.ico**——Win32 的 ExtractIconEx 只认 .ico/.exe/.dll，给 PNG 会拿不到图标。
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/l2dpet.png ${CMAKE_CURRENT_BINARY_DIR}/l2dpet.png COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/l2dpet.ico ${CMAKE_CURRENT_BINARY_DIR}/l2dpet.ico COPYONLY)
# 开发者 logo（关于页显示；EUI 的图片源按"程序旁边的相对路径"找）
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/l2dpet-logo.png ${CMAKE_CURRENT_BINARY_DIR}/l2dpet-logo.png COPYONLY)
# 多配置生成器（Visual Studio / Xcode）下 CMAKE_CURRENT_BINARY_DIR 不是 exe 所在目录
# （那是 <build>/Release/），所以再补一条 post-build 拷到 exe 旁边——
# 少这一步的表现是"程序编出来了，但托盘图标找不到"。
add_custom_command(TARGET l2dpet-ui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/l2dpet.png
          $<TARGET_FILE_DIR:l2dpet-ui>/l2dpet.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/l2dpet-logo.png
          $<TARGET_FILE_DIR:l2dpet-ui>/l2dpet-logo.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/l2dpet.ico
          $<TARGET_FILE_DIR:l2dpet-ui>/l2dpet.ico)
