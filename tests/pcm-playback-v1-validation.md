# PCM playback v1 — 2026-09-24

工程：`D:\esp32s3v1.0\korvo_agent`，Korvo-2 V3.1 / ESP-IDF 5.4.4。

## 修改

- CPU 240 MHz，应用与 IDF 使用性能编译 -O2；micro-opus 保留其组件的优化选项。
- 接收/解码任务固定 CPU1、优先级 4；新增 CPU1、优先级 6 的 PCM 输出任务。
- 固定分配 500 ms 单声道 PCM 环形缓冲及 Opus 解码临时缓冲；预缓冲 150 ms。
  生产者有背压，最多等待 2 秒，不覆盖尚未播放的数据。播放中短缺时渐变到静音，
  重新积累预缓冲后恢复。短回答在 EOF 到达后不受预缓冲阈值限制。
- 按 10 ms 块持续写入 I2S，空闲时写静音；DMA 为 6×10 ms。
- 起音 5 ms 渐入；断音/打断以最多一个 10 ms 块渐出。取消清除软件 PCM，随后
  写入静音覆盖 DMA 排队窗口，不关闭与麦克风共享的 TX/RX 时钟。
- 服务端结束事件只标记 EOF；等软件 PCM 与 DMA 尾音排空，再发布正常完成事件。
  等待尾音期间仍允许主动取消；输出错误不视为正常完成。
- AEC 数字参考改为在输出任务提交 I2S 时更新（包含静音），按配置的 60 ms DMA
  窗口保留待播样本，丢弃过旧历史。此为软件时序估计，并非硬件回采；实际采集、
  扬声器声学延迟仍需板上测量。
- AFE feed 每帧后让出一 tick（当前 tick=1 ms），并每 5 秒报告处理耗时。
- 扬声器音量仍为 40。唤醒门限、连续对话超时与鉴权配置未改。

## 构建兼容

安装的 Xtensa GCC 14.2.0 对 ESP-DSP 1.8.0 的
`modules/conv/float/dspi_conv_f32_ansi.c` 在 -O2 的寄存器分配阶段发生内部崩溃。
已验证 -O1 可编译。项目 CMakeLists.txt 对该编译器版本的这个源文件单独追加 -O1；
未编辑 managed_components，也未对全部音频代码降级优化。

## 验证

- 使用 `idf.py -DCCACHE_ENABLE=0 build` 完整构建，最终退出码 0。
- 最终增量构建日志：`work/pcm-playback-final-build.log`。
- 生成配置与 compile_commands 已确认 CPU=240、应用源文件 -O2，以及单个 DSP
  文件的 -O1 例外。
- `pcm_policy_test.c` 覆盖预缓冲、短回答部分块、参考窗口和负采样值渐变。
- `playback_completion_test.c` 覆盖 EOF 未排空、正常完成一次、排空期间打断及下一轮。
- 上述测试及现有 response_flow、barge_in_guard 测试使用 ESP Clang -O2 编译为 LLVM IR，
  main 全部折叠为 ret i32 0。属于编译期确定性逻辑验证，不是板上执行或调度测试。
- 应用镜像 2,567,600 字节（0x272db0）；3 MiB 应用分区余 0x8d250 字节（18%）。
  bootloader 容量检查通过。
- 尚未烧录、未执行声学测试。不能据此宣称断续/爆音或 AEC 已在实机解决。

启动标识：`build=pcm-playback-v1`

ELF SHA256：`EB63B370EBB7F6B0ED9DF3291EF2DC8F3322F211189C96F14351AD8DF8940AF7`

BIN SHA256：`33C96200B7C652653F46B8752DA3139DBC831639DD66967215636E984ABAED6F`

原文件备份：`work/before_pcm_playback_20260924_153758`。

## 实机验收

在本工程的 ESP-IDF 终端执行 `idf.py flash monitor`，必要时加 `-p COM端口号`。
确认启动标识和 CPU 240000000 Hz；首先静默播放多段长回答，再测试短回答、连续问答、
主动打断后立即提问。仍有异常时保留从启动至异常后的完整日志。

- `Playback stats`: buffered_ms 为软件 PCM 量；starvations 是软件供数短缺次数，
  并非硬件 DMA 欠载计数。decode_max_us 是本统计窗口单包解码最大耗时，write_max_us
  包含 I2S 的正常阻塞等待，不能直接把等待时间当作 CPU 运算耗时。
- `AFE stats`: feed_max_us 与 budget_us=64000 对比，over_budget 统计超预算帧数。
- `Response received; draining PCM tail` 仅表示收完；`Playback drained` 才表示本地排空。
- 若 starvations 持续增加或 AFE 经常超预算，需根据统计进一步调节处理负载或任务分配。
  若计数正常仍爆音，下一步检查实际 I2S 波形、采样削波、功放与供电。
- 正常起播增加约 150 ms 预缓冲和 DMA 排队时间；有意用少量延迟换取连续性。
