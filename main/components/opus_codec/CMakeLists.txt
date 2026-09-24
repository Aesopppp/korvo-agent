# CMakeLists.txt — opus_codec (本地组件代理)
#
# 依赖: esphome/micro-opus 组件 (Xtensa DSP 优化 + PSRAM 支持)
#
# ESP-IDF 组件注册: 声明依赖 micro-opus (由 component registry 自动下载)
idf_component_register(
    INCLUDE_DIRS "${COMPONENT_DIR}/include"
    REQUIRES micro-opus
)
