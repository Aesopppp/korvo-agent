# 打断后恢复修复（2026-09-24）

工程：D:\esp32s3v1.0\korvo_agent
修改前备份：work\before_recovery_20260924

## 行为变化

- WebSocket 收包回调仅组帧入队；解析、解码和 I2S 写入在独立任务执行，避免阻塞客户端锁。
- 打断后先停止播放、发送 response.cancel；收到旧回答结束确认后才恢复麦克风上传，避免旧回答结束事件再次启动监听。
- 取消确认等待 3 秒；连接或认证、上传或回复失败进入显式断开、5 秒重连和重新认证流程。
- 使用连接代数丢弃旧连接的事件。认证前不上传音频；活动对话在恢复窗口 60 秒内认证成功后恢复监听。
- 播放时保存约半秒最新麦克风音频；取消等待期间缓冲更多音频，确认后上传。重新建立云端会话时清空旧音频，用户可能需要重述问题。
- 录音任务未退出时不释放编码器。

## 已验证

本环境 idf.py / Ninja 实际构建受到 Windows 子进程管道限制，因此按 CMake 生成的编译参数直接编译了 main 下全部 C 文件，并执行生成的归档、链接脚本生成、链接、elf2image 和分区检查命令。

- 应用 C 文件编译通过。
- ELF 链接、ESP32-S3 镜像生成通过。
- 应用镜像 0x278ff0 字节；3 MiB 应用分区剩余 18%。
- Bootloader 大小检查通过。
- tests/response_flow_test.c 覆盖连续问答、取消期间的迟到音频、重复结束事件、取消后新回答以及重连状态重置。ESP Clang -O2 生成的 LLVM IR 将测试 main 完整化简为 ret i32 0；这是编译器静态求值验证，不是开发板或主机运行测试。

## 烧录后验收

1. 等到 WebSocket authenticated; audio upload ready，再唤醒；连续提问至少三次。
2. 回答期间讲话打断：应出现 Playback stopped; waiting for cancelled reply boundary，随后 Cancel acknowledged; resume buffered microphone audio 和 Conversation listening; ready for the next question。
3. 若服务端未确认取消，应出现 Session recovery: cancel acknowledgement timed out，随后重连认证并恢复监听，不应长期停在播放状态。
4. 暂时关闭路由器再恢复，确认认证后恢复监听；超过恢复窗口则重新唤醒。
5. 停止说话一分钟，确认回到 WAIT_WAKE。

本次没有修改 AEC 算法、参考通道或误打断阈值，也没有烧录、执行上述硬件测试或上传 GitHub。扬声器声音误触发打断的问题仍需实机检查回采参考与麦克风信号。