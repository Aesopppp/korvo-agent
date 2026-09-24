/**
 * @file wake_word.h
 * @brief 唤醒词检测模块 (esp-sr WakeNet)
 *
 * 负责：
 *  - AFE (Audio Front-End) 初始化与 I²S 喂流
 *  - WakeNet 唤醒词检测
 *  - PCM ring buffer 共享给 audio_in
 *
 * 架构说明：
 *  wake_word.c 启动后持续运行 feed_Task → AFE → ring buffer。
 *  audio_in 在唤醒后从同一个 ring buffer 读取 PCM，自身不初始化 I²S。
 *  detect_Task 监听 WAKENET_DETECTED 事件，通过事件总线通知 app_main。
 *
 * 状态机：
 *  WAIT_WAKE → (唤醒词检测到) → 触发 EV_AUDIO_WAKE_DETECTED → app_main 切到 LISTENING
 *
 * 依赖：esp-sr v2.1.3，ES7210 ADC，I²S0 (GPIO9/10/16/45)
 */
#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 常量配置
 * ========================================================================= */

/** About ten seconds in PSRAM, including per-item ring-buffer headers. Keeps
 * the start of a barge-in utterance during the eight-second cancel deadline. */
#define WAKE_WORD_RB_ITEM_SIZE  512   /* 512 × int16 = 1024 bytes/帧 */
#define WAKE_WORD_RB_ITEM_COUNT 320
#define WAKE_WORD_RB_TOTAL_SIZE ((WAKE_WORD_RB_ITEM_SIZE * sizeof(int16_t) + 8) * WAKE_WORD_RB_ITEM_COUNT)

/** feed_Task 栈深度 (AFE I²S 喂流，与参考例程一致) */
#define WAKE_WORD_FEED_STACK    (8 * 1024)

/** detect_Task 栈深度 (WakeNet 检测) */
#define WAKE_WORD_DETECT_STACK  (4 * 1024)

/* ===========================================================================
 * 类型定义
 * ========================================================================= */

/** 唤醒词检测状态 */
typedef enum {
    WAKE_STATE_IDLE = 0,   /**< 空闲（AFE 正常运行，仅监听唤醒词）*/
    WAKE_STATE_DETECTED,   /**< 唤醒词检测到（已触发回调，尚未重置）*/
    WAKE_STATE_PROCESSING,  /**< 唤醒后处理中（audio_in 正在收音）*/
} wake_word_state_t;

/** 唤醒词检测结果回调
 *  @param wake_word_index  唤醒词在模型中的索引
 *  @param wakenet_model_index WakeNet 模型索引
 *  @param user_data        注册时传入的上下文
 */
typedef void (*wake_word_detected_cb_t)(int wake_word_index,
                                        int wakenet_model_index,
                                        void *user_data);

/* ===========================================================================
 * API
 * ========================================================================= */

/**
 * @brief 初始化唤醒词检测（启动 AFE feed_Task + detect_Task）
 *
 * 调用时机：在所有其他服务初始化之后、状态机启动之前。
 * 内部创建两个 FreeRTOS 任务（core 0/1），持续运行直到 wake_word_deinit()。
 *
 * @return ESP_OK 成功；ESP_FAIL 失败（模型未找到或 AFE 初始化失败）
 */
esp_err_t wake_word_init(void);

/**
 * @brief 启动唤醒词检测任务（feed + detect）
 * @note 若尚未初始化，自动调用 wake_word_init()；
 *       若任务已在运行，为 no-op。
 */
esp_err_t wake_word_start(void);

/**
 * @brief 停止唤醒词检测任务（保留 AFE handle，可快速 restart）
 * @note 仅停止任务；AFE 和 ring buffer 保留，wake_word_start() 可重新启动。
 *       完整释放资源请用 wake_word_deinit()。
 */
void wake_word_stop(void);

/**
 * @brief 反初始化唤醒词检测（停止任务，释放 AFE 和 ring buffer）
 */
void wake_word_deinit(void);

/**
 * @brief 获取 AFE PCM ring buffer 句柄（供 audio_in.c 使用）
 *
 * @return RingbufHandle_t，唤醒词初始化前为 NULL
 */
RingbufHandle_t wake_word_get_ringbuf(void);
/** Flush stale PCM with the upload task stopped. */
void wake_word_clear_audio_buffer(void);

/**
 * @brief 获取 AFE fetch 帧大小（samples）
 *
 * @return 每帧 sample 数（16kHz 典型值 512 = 32ms）
 */
int wake_word_get_chunksize(void);

/**
 * @brief 获取当前唤醒状态
 */
wake_word_state_t wake_word_get_state(void);

/**
 * @brief 注册唤醒词检测回调（触发后 app_main 切换到 LISTENING）
 *
 * @param cb        回调函数（不可 NULL）
 * @param user_data 透传给回调的上下文
 */
void wake_word_set_callback(wake_word_detected_cb_t cb, void *user_data);

/**
 * @brief 重置唤醒状态（从 DETECTED → IDLE，准备下一轮唤醒检测）
 *
 * detect_Task 在触发回调后自动调用，或由 app_main 在链路结束时手动调用。
 */
void wake_word_reset(void);

/** Clear the one-shot barge-in latch after the cancelled response is done. */
void wake_word_reset_barge_in(void);

/**
 * @brief 获取 feed_Task 栈高水位（调试用）
 */
uint32_t wake_word_get_feed_stack_hiwat(void);

/**
 * @brief 获取 detect_Task 栈高水位（调试用）
 */
uint32_t wake_word_get_detect_stack_hiwat(void);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H */
