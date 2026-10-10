# Live2D 最小播放器（Cubism Framework + RK3506 软渲染后端）
#
# 不直接 cmake 本文件——用 live2d/build-player.sh（它负责组装构建树并选平台）。
# 手动构建时需指定 CSM_BUILD_ROOT = 含 `Core/` 与 `Framework/` 的构建根
# （Framework/src/Rendering/ 下需已注入 Software/ 软渲染后端目录）。

cmake_minimum_required(VERSION 3.10)
project(l2dplayer CXX)

# ---- Windows：静态 CRT（/MT）----
# 产物不依赖 MSVCP140/VCRUNTIME140/VCRUNTIME140_1.dll —— 干净的 Windows 上双击就能跑，
# 不用先装 VC++ 运行库（剩下的都是系统 DLL：kernel32/user32/gdi32/opengl32…）。
# 手法是直接改编译 flag，而不是 CMP0091/CMAKE_MSVC_RUNTIME_LIBRARY：vendored 的
# glfw/freetype/curl 各自的 cmake_minimum_required 会把该策略在它们的目录里重置回旧行为，
# 只对顶层生效的写法会漏掉三方库（混用 /MD 与 /MT 直接 LNK2038 链接失败）。
if(MSVC)
  foreach(_crt_var
          CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
          CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
          CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
          CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REGEX REPLACE "/MD" "/MT" ${_crt_var} "${${_crt_var}}")
  endforeach()
  set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)   # GLFW 自己那套开关（默认 DLL 版）
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CSM_BUILD_ROOT)
  message(FATAL_ERROR "CSM_BUILD_ROOT 未设置：请用 live2d/build-player.sh 构建")
endif()

# ---- Cubism Core（预编译库；按目标平台选）----
# 优先用**仓库自带**的 Core（live2d/3rd/CubismCore：头文件 + 三端 lib/dll，随仓库分发，
# clone 下来就能编）；仓库里没有才退回 CSM_BUILD_ROOT 里的 SDK 副本（外部 SDK / 老流程）。
set(L2D_CORE_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/CubismCore" CACHE PATH "仓库自带的 Cubism Core（include/ + lib/ + dll/）")

# 取第一个存在的候选路径；一个都没有就返回最后一个（让报错指向一个像样的位置）
function(l2d_pick_existing out)
  set(_fallback "")
  foreach(_p ${ARGN})
    set(_fallback "${_p}")
    if(EXISTS "${_p}")
      set(${out} "${_p}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "${_fallback}" PARENT_SCOPE)
endfunction()

l2d_pick_existing(_csm_core_inc
  ${L2D_CORE_VENDOR}/include
  ${CSM_BUILD_ROOT}/Core/include)

if(WIN32)
  # Windows：SDK 按工具集（141/142/143）× CRT（MD/MT）× 配置（d）分目录放。
  # 我们用 /MT（静态 CRT，见文件开头），所以取 _MT/_MTd——拿 _MD 版会和 /MT 目标
  # 冲突（重复运行库 → LNK2038）。仓库自带的那份放在与工具集无关的平铺目录。
  l2d_pick_existing(_csm_core_mt_rel
    ${L2D_CORE_VENDOR}/lib/windows/x86_64/Live2DCubismCore_MT.lib
    ${CSM_BUILD_ROOT}/Core/lib/windows/x86_64/Live2DCubismCore_MT.lib
    ${CSM_BUILD_ROOT}/Core/lib/windows/x86_64/143/Live2DCubismCore_MT.lib
    ${CSM_BUILD_ROOT}/Core/lib/windows/x86_64/Live2DCubismCore.lib)
  l2d_pick_existing(_csm_core_mt_dbg
    ${L2D_CORE_VENDOR}/lib/windows/x86_64/Live2DCubismCore_MTd.lib
    ${CSM_BUILD_ROOT}/Core/lib/windows/x86_64/Live2DCubismCore_MTd.lib
    ${CSM_BUILD_ROOT}/Core/lib/windows/x86_64/143/Live2DCubismCore_MTd.lib
    ${_csm_core_mt_rel})
  set(_csm_core_lib ${_csm_core_mt_rel})
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
    set(_csm_mac_arch arm64)
  else()
    set(_csm_mac_arch x86_64)
  endif()
  l2d_pick_existing(_csm_core_lib
    ${L2D_CORE_VENDOR}/lib/macOS/${_csm_mac_arch}/libLive2DCubismCore.a
    ${CSM_BUILD_ROOT}/Core/lib/macOS/${_csm_mac_arch}/libLive2DCubismCore.a
    ${CSM_BUILD_ROOT}/Core/lib/macOS/libLive2DCubismCore.a)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
  l2d_pick_existing(_csm_core_lib
    ${CSM_BUILD_ROOT}/Core/lib/experimental/rpi/libLive2DCubismCore.a)
else()
  l2d_pick_existing(_csm_core_lib
    ${L2D_CORE_VENDOR}/lib/linux/x86_64/libLive2DCubismCore.a
    ${CSM_BUILD_ROOT}/Core/lib/linux/x86_64/libLive2DCubismCore.a)
endif()

if(NOT EXISTS ${_csm_core_lib})
  message(FATAL_ERROR "找不到 Cubism Core：${_csm_core_lib}\n"
                      "仓库自带一份在 live2d/3rd/CubismCore（正常不该缺）；\n"
                      "用外部 SDK 时把 CSM_BUILD_ROOT 指向含 Core/ 与 Framework/ 的构建根。")
endif()

add_library(Live2DCubismCore STATIC IMPORTED)
if(WIN32 AND EXISTS ${_csm_core_mt_rel})
  # Release/Debug 各取对版的 CRT（/MT ↔ _MT.lib，/MTd ↔ _MTd.lib），混了就是 LNK2038
  set_target_properties(Live2DCubismCore PROPERTIES
    IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
    IMPORTED_LOCATION_RELEASE ${_csm_core_mt_rel}
    IMPORTED_LOCATION_DEBUG ${_csm_core_mt_dbg}
    MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
    INTERFACE_INCLUDE_DIRECTORIES ${_csm_core_inc})
else()
  set_target_properties(Live2DCubismCore PROPERTIES
    IMPORTED_LOCATION ${_csm_core_lib}
    INTERFACE_INCLUDE_DIRECTORIES ${_csm_core_inc})
endif()

# ---- 统一配置（l2d_config.h；对齐 LVGL lv_conf.h 用法）----
# 默认用 player/l2d_config.h；可 -DL2D_CONFIG_FILE=/path/to/l2d_config.h 覆盖
set(L2D_CONFIG_FILE "" CACHE FILEPATH "自定义 l2d_config.h 路径（默认 player/l2d_config.h）")
if(L2D_CONFIG_FILE)
  get_filename_component(CSM_L2D_CONFIG_DIR ${L2D_CONFIG_FILE} DIRECTORY)
  set(CSM_L2D_CONFIG_FALLBACK_DIR ${CMAKE_CURRENT_SOURCE_DIR}) # 供 #include_next 拉默认值
  message(STATUS "l2d 配置: ${L2D_CONFIG_FILE}")
else()
  set(CSM_L2D_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(CSM_L2D_CONFIG_FALLBACK_DIR "")
  message(STATUS "l2d 配置: ${CSM_L2D_CONFIG_DIR}/l2d_config.h（仓库默认）")
endif()

# ---- Cubism Framework（开源 C++；FRAMEWORK_SOURCE 选我们的软件渲染后端）----
set(FRAMEWORK_SOURCE Software)
add_subdirectory(${CSM_BUILD_ROOT}/Framework ${CMAKE_CURRENT_BINARY_DIR}/framework)

# ---- 最小播放器 ----
set(L2D_SOURCES
  src/l2dtest.cpp
  # 平台端口抽象（桌面三端：x11 / pet / glfwpet / sdl / headless，最差纯软解算）
  src/Port.hpp
  src/Port.cpp
  src/PortHeadless.cpp
  src/PortX11.cpp
  src/PortX11Pet.cpp
  src/PortSdl.cpp
  src/PortGlfwPet.cpp
  src/PetText.h
  src/PetText.cpp
  src/PetPaint.h
  src/CtlServer.cpp
  src/CtlServer.hpp
  # 跨平台小工具层（时间/路径/目录/子进程）
  src/Platform.h
  src/Platform.cpp
  # 内置 agent（LLM 大脑：LlmClient 走 curl，PetMemory 跨会话记忆，PetAgent 线程+节制）
  src/LlmClient.h
  src/LlmClient.cpp
  src/PetMemory.h
  src/PetMemory.cpp
  # 每个模型的性格（模型能力 → LLM 生成 → 按指纹落盘 → 注入提示词）
  src/PetPersona.h
  src/PetPersona.cpp
  # 问题库（想了解主人的问题：多题材生成 + 已问/已答跟踪，避免总问"在干什么"）
  src/PetQuestions.h
  src/PetQuestions.cpp
  # 笑话池（LLM 定期增量补货；桌宠本地念，秒回）
  src/PetJokes.h
  src/PetJokes.cpp
  src/PetAgent.h
  src/PetAgent.cpp
  src/PetWeather.h
  src/PetWeather.cpp
  # 桌宠自带文案的 i18n（气泡台词 / 动态描述；LLM 那份由 agent 按语言生成）
  src/PetI18n.h
  src/PetI18n.cpp
  # 从 l2dtest.cpp 拆出的模块（见各自 .h 的说明；纯搬移，逻辑未改）
  src/SysStats.h
  src/PlayerAllocator.h
  src/FileUtil.cpp
  src/ModelImport.cpp
  src/Backup.cpp
  src/PpmPlayer.cpp
  # L2DPlayer（播放器主体）：类声明 + 数据成员 + 小访问器在头里，大函数按职责分在下面 6 个文件
  src/L2DPlayer.h
  src/PlayerCore.cpp
  src/PlayerCmd.cpp
  src/PlayerBody.cpp
  src/PlayerCfg.cpp
  src/PlayerLife.cpp
  src/PlayerProbe.cpp
  # main 的两半：命令行选项解析 + 运行期（装载/端口/主循环）
  src/PetBootstrap.h
  src/PetBootstrap.cpp
  src/MainLoop.h
  src/MainLoop.cpp
)
if(UNIX)
  # 桌面端口（X11 / GLFW pet）在下面按依赖可用性打开；macOS 走 GLFW。
endif()
add_executable(l2dtest ${L2D_SOURCES})

# X11 端口（Linux 桌面：桌宠窗口 + 气泡；头文件通常现成，缺失则跳过该 port）
option(L2D_ENABLE_X11 "构建 X11 端口（Linux 桌面）" ON)
if(L2D_ENABLE_X11)
  find_package(X11 QUIET)
else()
  set(X11_FOUND FALSE)
endif()
if(X11_FOUND)
  target_compile_definitions(l2dtest PRIVATE L2D_WITH_X11=1)
  target_include_directories(l2dtest PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(l2dtest PRIVATE ${X11_LIBRARIES})
  message(STATUS "X11 port 已启用")
  if(X11_Xext_LIB AND X11_Xshape_FOUND)
    # 气泡（Xft 文字渲染 + fontconfig）是桌宠的一部分：缺 Xft 就不编 pet port
    find_package(PkgConfig QUIET)
    set(L2D_XFT_OK FALSE)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(XFT QUIET xft fontconfig)
      if(XFT_FOUND)
        set(L2D_XFT_OK TRUE)
        target_include_directories(l2dtest PRIVATE ${XFT_INCLUDE_DIRS})
        target_link_libraries(l2dtest PRIVATE ${XFT_LIBRARIES})
      endif()
    endif()
    if(L2D_XFT_OK)
      target_compile_definitions(l2dtest PRIVATE L2D_WITH_X11_PET=1)
      target_link_libraries(l2dtest PRIVATE ${X11_Xext_LIB})
      # 托盘 / 右键菜单 / 气泡 / 设置窗口都在 l2dpet-ui 进程（live2d/panel/，EUI-NEO）
      message(STATUS "桌宠 port（pet：透明置顶 + XShape 点击穿透 + Xft 气泡）已启用")
    else()
      message(STATUS "缺 Xft/fontconfig——pet port 不可用（装 libxft-dev libfontconfig-dev）")
    endif()
  endif()
else()
  message(STATUS "未找到 X11——x11 port 不可用（headless/glfw 不受影响）")
endif()

# SDL2 端口（跨平台桌面）：需 SDL2 开发包，编译期可选
find_package(SDL2 QUIET)
if(SDL2_FOUND)
  target_compile_definitions(l2dtest PRIVATE L2D_WITH_SDL=1)
  target_include_directories(l2dtest PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(l2dtest PRIVATE ${SDL2_LIBRARIES})
  message(STATUS "SDL2 port 已启用")
else()
  message(STATUS "未找到 SDL2 开发包——sdl port 不可用（x11/headless 不受影响）")
endif()

# 内置 agent 的 LLM HTTP(S)：libcurl（可选——缺了 LlmChat 直接报错，其余功能不受影响）
# Linux/macOS 走 pkg-config 找系统 libcurl；Windows 没有 pkg-config，见文件末尾的
# "内置 libcurl" 分支（拉官方源码包就地编静态版）。
find_package(PkgConfig QUIET)
set(L2D_CURL_OK FALSE)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBCURL QUIET libcurl)
  if(LIBCURL_FOUND)
    target_include_directories(l2dtest PRIVATE ${LIBCURL_INCLUDE_DIRS})
    target_link_libraries(l2dtest PRIVATE ${LIBCURL_LIBRARIES})
    target_compile_definitions(l2dtest PRIVATE L2D_WITH_CURL=1)
    set(L2D_CURL_OK TRUE)
    message(STATUS "内置 agent：libcurl 已启用（https 可用）")
  else()
    message(STATUS "缺 libcurl 开发包——内置 agent 的 LLM 调用不可用（装 libcurl4-openssl-dev）")
  endif()
endif()

# ---- 跨平台桌宠端口 PortGlfwPet：GLFW 3.4 + glad + freetype（三端同一份代码）----
# 三方源码默认取**仓库自带的本地副本**（EUI-NEO 的 3rd/，随仓库分发，不需要 git/网络）；
# 想用别处的：-DL2D_3RD_DIR=<目录>。
set(L2D_3RD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd/EUI-NEO/3rd" CACHE PATH "vendored 三方源码目录（GLFW/glad/freetype；默认用仓库自带的本地副本）")
option(L2D_ENABLE_GLFW_PET "构建 GLFW 跨平台桌宠端口（glfwpet）" ON)
if(L2D_ENABLE_GLFW_PET)
  set(_l2d_have_3rd TRUE)
  foreach(_dep glfw glad freetype)
    if(NOT EXISTS "${L2D_3RD_DIR}/${_dep}")
      set(_l2d_have_3rd FALSE)
    endif()
  endforeach()
  if(_l2d_have_3rd)
    if(NOT TARGET glfw)
      set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
      set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
      add_subdirectory("${L2D_3RD_DIR}/glfw" "${CMAKE_CURRENT_BINARY_DIR}/_deps/glfw" EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET glad)
      add_subdirectory("${L2D_3RD_DIR}/glad" "${CMAKE_CURRENT_BINARY_DIR}/_deps/glad" EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET freetype)
      set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
      set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
      set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
      set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
      set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
      set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
      set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
      add_subdirectory("${L2D_3RD_DIR}/freetype" "${CMAKE_CURRENT_BINARY_DIR}/_deps/freetype" EXCLUDE_FROM_ALL)
    endif()
    target_link_libraries(l2dtest PRIVATE glfw glad freetype)
    target_compile_definitions(l2dtest PRIVATE L2D_WITH_GLFW_PET=1)
    message(STATUS "桌宠 port（glfwpet：GLFW/glad/freetype 跨平台）已启用")
  else()
    message(STATUS "缺 vendored GLFW/glad/freetype（${L2D_3RD_DIR}）——glfwpet 端口不可用")
  endif()
endif()
target_include_directories(l2dtest BEFORE PRIVATE
  ${CSM_L2D_CONFIG_DIR}                                # l2d_config.h（可被 -DL2D_CONFIG_FILE 覆盖）
)
if(CSM_L2D_CONFIG_FALLBACK_DIR)
  # 仓库默认配置排在次位：供自定义配置里 #include_next "l2d_config.h" 拉取默认值
  target_include_directories(l2dtest PRIVATE ${CSM_L2D_CONFIG_FALLBACK_DIR})
endif()
target_include_directories(l2dtest PRIVATE
  ${CSM_BUILD_ROOT}/Core/include
  ${CSM_BUILD_ROOT}/Framework/src/Rendering/Software   # 软渲染后端头（注入副本）
)

# 三端链接：Linux(pthread/m) / Windows(ws2_32) / macOS(pthread 在 libc)
if(WIN32)
  target_link_libraries(l2dtest PRIVATE Framework Live2DCubismCore ws2_32 winmm)
elseif(APPLE)
  target_link_libraries(l2dtest PRIVATE Framework Live2DCubismCore "-framework CoreFoundation")
else()
  target_link_libraries(l2dtest PRIVATE Framework Live2DCubismCore pthread m)
endif()

# MSVC：源码与运行库按 UTF-8（中文台词/提示词字面量）；
# NOMINMAX 让 std::min/std::max 不被 <windows.h> 的 min/max 宏顶掉
# （Platform.cpp / PortGlfwPet.cpp 会引入 windows.h）
if(MSVC)
  target_compile_options(l2dtest PRIVATE /utf-8)
  target_compile_definitions(l2dtest PRIVATE NOMINMAX)
endif()

# Windows：GUI 子系统——双击桌宠**不带黑窗口**（桌宠是常驻窗口程序，控制台只是日志）。
# 入口用 l2dtest.cpp 里的 WinMain → main（**不用** /ENTRY:mainCRTStartup 硬改入口点：
# 那样绕开 CRT 的 GUI 启动路径，静态 CRT + 无控制台时 stdout/stderr 会写不出去——日志只剩零星几行，实测）。
# 从命令行/带重定向启动时 stdout/stderr 依旧有效，日志照旧进调用方的控制台或文件。
# 没控制台时日志落到 %TEMP%\l2dpet.log（l2dtest.cpp 的 AttachLogIfHeadless）。
option(L2D_WIN_GUI_SUBSYSTEM "Windows：l2dtest 用 GUI 子系统（双击不带黑窗口）" ON)
if(WIN32 AND MSVC AND L2D_WIN_GUI_SUBSYSTEM)
  set_target_properties(l2dtest PROPERTIES WIN32_EXECUTABLE TRUE)
  target_compile_definitions(l2dtest PRIVATE L2D_GUI_SUBSYSTEM=1)
endif()

# ---- 内置 libcurl（Windows）----
# Windows 上没有 pkg-config / 系统 libcurl，官方给的又是 mingw 版（MSVC 链不上），
# 所以这里拉官方源码包就地编一份**静态 + Schannel** 的：
#   · 静态 → 生成的 l2dtest.exe 不依赖 libcurl.dll，绿色版/安装包都不用额外带文件；
#   · Schannel → 用 Windows 原生 TLS，不引入 OpenSSL/mbedTLS；
#   · HTTP_ONLY → LLM 客户端只要 HTTP(S)，顺手砍掉 FTP/LDAP/SSH 这些外部依赖。
# 源码与编译产物都落在构建树（_deps/），只在第一次配置时下一遍（约 4MB）。
# 放在文件最后：curl 也是 CMake 工程，避免它的缓存变量（BUILD_SHARED_LIBS 等）
# 影响前面刚加进来的 glfw/freetype。
if(NOT L2D_CURL_OK AND WIN32)
  set(L2D_CURL_VERSION "8.11.1")
  message(STATUS "内置 agent：拉取 libcurl ${L2D_CURL_VERSION} 源码（静态 + Schannel，仅首次）")
  include(FetchContent)
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
  set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
  set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(HTTP_ONLY ON CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_STATIC_CRT ON CACHE BOOL "" FORCE)   # 与顶层 /MT 一致（curl 有自己的开关）
  set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
  FetchContent_Declare(curl
    URL "https://curl.se/download/curl-${L2D_CURL_VERSION}.tar.gz"
    URL_HASH SHA256=a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(curl)
  target_link_libraries(l2dtest PRIVATE CURL::libcurl)
  # 静态链 libcurl 时头文件里不能按 dllimport 声明（CURL_STATICLIB 就是干这个的）
  target_compile_definitions(l2dtest PRIVATE L2D_WITH_CURL=1 CURL_STATICLIB=1)
  message(STATUS "内置 agent：libcurl 已启用（内置静态编译，https 可用）")
endif()
