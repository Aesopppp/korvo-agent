/**
 * @file board_config.h
 * @brief ESP32-S3-Korvo-2 V3.1.2 - *
 * SCH_ESP32-S3-Korvo-2_V3.1.2_20240116.pdf
 * IDF SP-IDF v5.4.4
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * I2C - (GPIO17/18) -ES7210 ADC + ES8311 DAC + TCA9554 IO-
 * V3.1.2  GPIO17=SDA, GPIO18=SCL
 * ========================================================================= */
#define BSP_I2C_SDA_PIN          (GPIO_NUM_17)
#define BSP_I2C_SCL_PIN          (GPIO_NUM_18)
#define BSP_I2C_PORT             I2C_NUM_0
#define BSP_I2C_FREQ_HZ          400000   /* 400 kHz Fast Mode */

/* ===========================================================================
 * I2S1 - -ES7210 ADC (-) + ES8311 DAC (-)
 * --: std/philips, 16-bit, 16 kHz, MONO)
 *
 * (-V3.1.2 :
 *   MCLK  <- GPIO16 ( ES7210/ES8311 -)
 *   BCLK  <- GPIO9  (
 *   WS    <- GPIO45 (L/R -, 16 kHz = 1/32 -)
 *   DIN   <- GPIO10 (ES7210 SDOUT -ESP32 I2S1 SDIN)
 *   DOUT  <- GPIO8  (ESP32 I2S1 SDOUT -ES8311 SDIN)
 * ========================================================================= */
#define BSP_I2S_PORT             I2S_NUM_1

#define BSP_I2S_MCLK_PIN         (GPIO_NUM_16)
#define BSP_I2S_BCLK_PIN         (GPIO_NUM_9)
#define BSP_I2S_WS_PIN           (GPIO_NUM_45)
#define BSP_I2S_DIN_PIN          (GPIO_NUM_10)  /* ES7210_SDOUT -ESP32 I2S0 DIN */
#define BSP_I2S_DOUT_PIN         (GPIO_NUM_8)   /* ESP32 I2S0 DOUT -ES8311 SDIN */

/* I2S  */
#define BSP_I2S_SAMPLE_RATE      16000
#define BSP_I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define BSP_I2S_CHANNEL_FORMAT  I2S_CHANNEL_FMT_ONLY_LEFT  /* ES7210 MONO */
#define BSP_I2S_MODE            (I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX)
#define BSP_I2S_STD_SLOT_MODE   I2S_SLOT_MODE_16BIT

/* I2S MCLK : MCLK = 16000 * 32 = 512000 Hz ( ES7210/ES8311 -) */
#define BSP_I2S_MCLK_DIV        4   /* PLL_F40M / 4 = 10 MHz ... - */
#define BSP_I2S_BCLK_DIV        32  /* BCLK = 16000 * 32 = 512000 / 32 = 16 kHz */

/* ===========================================================================
 *  * ========================================================================= */
#define BSP_PA_EN_PIN            (GPIO_NUM_48)  /* NS4150  (- */
/* Peripheral power is controlled by TCA9554 P5, not GPIO21. */

/* ===========================================================================
 * ADC - (GPIO5)
 * ========================================================================= */
#define BSP_ADC_KEY_PIN          (GPIO_NUM_5)
#define BSP_ADC_KEY_CHANNEL     ADC_CHANNEL_4  /* GPIO5 = ADC1_CH4 */

/* ===========================================================================
 * - UART (GPIO43/44) --
 * ========================================================================= */
#define BSP_UART_TX_PIN          (GPIO_NUM_43)
#define BSP_UART_RX_PIN          (GPIO_NUM_44)
#define BSP_UART_PORT            UART_NUM_1
#define BSP_UART_BAUD            115200

/* ===========================================================================
 * TCA9554 IO - -LED - (I2C , - 0x20)
 * OI-1: TCA9554_I2C_ADDR  0x20 - 0x3F
 * ========================================================================= */
#define TCA9554_I2C_ADDR         0x20    /*  - */
#ifndef TCA9554_I2C_ADDR
#define TCA9554_I2C_ADDR         0x20
#endif

/* TCA9554 --- (IO0-IO7) */
#define TCA9554_PIN_LED_GREEN    6   /* LED1 */
#define TCA9554_PIN_LED_RED      7   /* LED2 */
#define TCA9554_PIN_PERI_PWR     5   /* PERI_PWR_ON; PA is GPIO48 on V3.1 */
#define TCA9554_PIN_SPARE_3      3   /* - */
#define TCA9554_PIN_SPARE_4      4   /* - */
#define TCA9554_PIN_SPARE_5      5   /* - */
#define TCA9554_PIN_SPARE_6      6   /* - */
#define TCA9554_PIN_SPARE_7      7   /* - */

/* TCA9554  (0=-, 1=-) -LED/PA_SD  */
#define TCA9554_DEFAULT_DIR       ((uint8_t)~((1 << TCA9554_PIN_LED_GREEN) | \
                                     (1 << TCA9554_PIN_LED_RED)   | \
                                     (1 << TCA9554_PIN_PERI_PWR)))

/* TCA9554 -(PA_SD=1  LED ) */
#define TCA9554_DEFAULT_OUT      0x00 /* P5 low: peripheral power ON; LEDs OFF */

/* ===========================================================================
 * ADC (- GPIO5)
 *  * ========================================================================= */
#define ADC_KEY_CHANNEL_ID       4   /* ADC1_CH4 = GPIO5 */

/* ===========================================================================
 * ES7210 ADC - (- espressif/esp_codec_dev --)
 * ========================================================================= */
/* ES7210 I2C -
 * V3.1.2  AD1=-(=0), AD0=-(=0)
 * -7bit addr = 0b1000000 = 0x40
 * (esp_codec_dev - esp_codec_dev_es7210.c - ES7210_ADDR_0 = 0x40)
 */
#define ES7210_I2C_ADDR          0x40

/* ===========================================================================
 * ES8311 DAC - (- espressif/esp_codec_dev --)
 * ========================================================================= */
#define ES8311_I2C_ADDR          0x18    /* ES8311 - I2C - 0x18 */

/* ===========================================================================
 * Wi-Fi / - NVS- * ========================================================================= */
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define WIFI_MAX_RETRY           5
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""

/* ===========================================================================
 * ACOS
 * ========================================================================= */
#define WS_SERVICE_DEFAULT_HOST   "acos-platform.emicloud.com"
#define ACOS_WS_URL              "wss://acos-platform.emicloud.com/acos-realtime"
#define DEFAULT_ACOS_TOKEN ""
#define ACOS_OPUS_BITRATE        24000   /* - Opus 24 kbps */
/* -: ACOS  20ms/320 - */
#define ACOS_OPUS_FRAME_MS_UPLOAD   20
/* -: 60ms Opus  */
#define ACOS_OPUS_FRAME_MS_DOWNLOAD 60
#define ACOS_OPUS_FRAME_MS          ACOS_OPUS_FRAME_MS_DOWNLOAD /* - */
#define ACOS_INPUT_MAX_MS        20000   /* - 20 s */
#define ACOS_AUTH_TIMEOUT_MS     10000   /* - __proxy_connected__ - */

/* API Key -: sk-{enterprise}-{32 chars} */
#define ACOS_TOKEN_PREFIX        "sk-"
#define ACOS_TOKEN_MIN_LEN       12

/* ===========================================================================
 * Opus
 * ========================================================================= */
#define OPUS_SAMPLE_RATE         16000
#define OPUS_CHANNELS            1
#define OPUS_COMPLEXITY          4       /* -0-10), -- */

/* ===========================================================================
 * VAD -
 * ========================================================================= */
#define VAD_FRAME_SIZE_MS        30      /* VAD  30 ms */
#define VAD_SILENCE_TIMEOUT_MS   800     /*  - */
#define VAD_MIN_UTTERANCE_MS     300     /* */

/* ===========================================================================
 * -
 * ========================================================================= */
#define AUDIO_IN_QUEUE_SIZE      8
#define AUDIO_OUT_QUEUE_SIZE     8
#define AUDIO_CHUNK_SIZE         (BSP_I2S_SAMPLE_RATE * VAD_FRAME_SIZE_MS / 1000 * 2)  /* 16-bit PCM, 30ms = 960 bytes */
#define AUDIO_WS_QUEUE_SIZE      16

/* ===========================================================================
 * FreeRTOS - * ========================================================================= */
#define TASK_STACK_AUDIO_IN      4096
#define TASK_STACK_AUDIO_OUT     4096
#define TASK_STACK_WS            6144
#define TASK_STACK_PROTO         4096
#define TASK_STACK_WIFI          4096

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CONFIG_H */


