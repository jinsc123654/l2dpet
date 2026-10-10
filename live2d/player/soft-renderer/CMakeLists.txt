# 软渲染后端 —— 被 build-player.sh 注入成 Framework/src/Rendering/Software/
# 与官方 OpenGL/D3D11/... 后端目录同构：往 Framework 这个 target 追加源码。
# 依赖注入的 third_party/stb_image.h（见 build-player.sh 的 stb 注入步骤）。

target_sources(${LIB_NAME}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/CubismRenderer_Software.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CubismRenderer_Software.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SoftTexture.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SoftTexture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SoftCompat.hpp
)

target_include_directories(${LIB_NAME}
  PRIVATE
    ${CSM_BUILD_ROOT}/third_party
    ${CSM_L2D_CONFIG_DIR}   # l2d_config.h（统一配置；由顶层 CMake 传入）
)

# 自定义配置时，默认配置目录排在次位（供 #include_next 拉默认值）
if(CSM_L2D_CONFIG_FALLBACK_DIR)
  target_include_directories(${LIB_NAME} PRIVATE ${CSM_L2D_CONFIG_FALLBACK_DIR})
endif()

# MSVC：源码是 UTF-8（中文注释 + 日志字符串），必须按 UTF-8 读，
# 否则在 GBK 代码页(936)下会被 C4819 判为乱码、字符串字面量也会走味。
if(MSVC)
  target_compile_options(${LIB_NAME} PRIVATE /utf-8)
endif()

# 软渲染热点：单独拉满优化（只影响本后端两个源文件，不动官方 Framework 的浮点语义）
# -ffast-math 对渲染可接受（输出量化到 8bit；不影响 moc3 解码数值）
# MSVC 侧只给 /O2（Release 默认等价，显式写出以对齐结构）；未加 /fp:fast ——
# 该平台未经基准验证，先不引入浮点重排风险。
if(MSVC)
  set_source_files_properties(
      ${CMAKE_CURRENT_SOURCE_DIR}/CubismRenderer_Software.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/SoftTexture.cpp
    PROPERTIES
      COMPILE_OPTIONS "/O2"
  )
else()
  set_source_files_properties(
      ${CMAKE_CURRENT_SOURCE_DIR}/CubismRenderer_Software.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/SoftTexture.cpp
    PROPERTIES
      COMPILE_OPTIONS "-O3;-ffast-math;-funroll-loops"
  )
endif()
