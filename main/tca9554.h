/**
 * @file tca9554.h
 * @brief TCA9554 8位 I2C GPIO 扩展芯片驱动
 *
 * 数据手册: TCA9554 (Texas Instruments)
 * I2C 地址: 0x3E / 0x3F (引脚 A0 选择, OI-1 待原理图确认)
 *
 * 寄存器:
 *   0x00 - Input Port             (只读)
 *   0x01 - Output Port            (R/W)
 *   0x02 - Polarity Inversion     (R/W)
 *   0x03 - Configuration (方向)   (R/W: 0=输出, 1=输入)
 */
#ifndef TCA9554_H
#define TCA9554_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TCA9554 寄存器地址 */
#define TCA9554_REG_INPUT_PORT       0x00
#define TCA9554_REG_OUTPUT_PORT      0x01
#define TCA9554_REG_POLARITY_INV     0x02
#define TCA9554_REG_CONFIG           0x03

/* TCA9554 I2C 地址: A0=GND → 0x3E, A0=VCC → 0x3F (OI-1 待确认) */
#ifndef CONFIG_TCA9554_I2C_ADDR
#define TCA9554_I2C_ADDR_DEFAULT     0x3E
#else
#define TCA9554_I2C_ADDR_DEFAULT     CONFIG_TCA9554_I2C_ADDR
#endif

/* TCA9554 句柄 */
typedef struct tca9554_dev *tca9554_handle_t;

/**
 * @brief 创建设备句柄
 * @param i2c_bus 已初始化的 I2C 总线句柄
 * @param addr    I2C 地址 (默认 0x3E)
 * @return 句柄，失败返回 NULL
 */
tca9554_handle_t tca9554_new(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

/**
 * @brief 销毁设备句柄
 */
void tca9554_delete(tca9554_handle_t dev);

/**
 * @brief 设置引脚方向
 * @param dev   句柄
 * @param mask  bit=1 的引脚设为输入，bit=0 设为输出
 * @return ESP_OK
 */
esp_err_t tca9554_set_direction(tca9554_handle_t dev, uint8_t mask);

/**
 * @brief 读取输入端口
 * @param dev  句柄
 * @param val  输出当前引脚状态
 */
esp_err_t tca9554_read_input(tca9554_handle_t dev, uint8_t *val);

/**
 * @brief 写入输出端口
 * @param dev   句柄
 * @param mask  bit=1 的引脚保持原值，仅写入 val 中 bit=1 的引脚
 * @param val   写入的值 (mask=1 的位有效)
 */
esp_err_t tca9554_write_output(tca9554_handle_t dev, uint8_t mask, uint8_t val);

/**
 * @brief 直接设置输出电平 (会修改整个端口)
 * @param dev   句柄
 * @param value 输出值 (8位)
 */
esp_err_t tca9554_set_output(tca9554_handle_t dev, uint8_t value);

/**
 * @brief 读取输出端口
 */
esp_err_t tca9554_read_output(tca9554_handle_t dev, uint8_t *val);

/**
 * @brief 初始化默认值 (方向 + 输出电平)
 * @param dev 句柄
 * @param dir  方向寄存器值 (默认: 低4位输出,高4位输入)
 * @param out  输出寄存器初值
 */
esp_err_t tca9554_init_defaults(tca9554_handle_t dev, uint8_t dir, uint8_t out);

#ifdef __cplusplus
}
#endif

#endif /* TCA9554_H */
