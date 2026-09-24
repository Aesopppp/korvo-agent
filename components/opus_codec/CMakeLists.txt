# CMakeLists.txt — opus_codec (本地组件代理)
#
# 依赖: esphome/micro-opus 组件 (Xtensa DSP 优化 + PSRAM 支持)
# opus_codec.c/h 源码在 main/ 中，此组件只负责声明对 micro-opus 的依赖
idf_component_register(
    REQUIRES micro-opus
)
