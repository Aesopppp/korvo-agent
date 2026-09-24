/**
 * @file tca9554.c
 * @brief TCA9554 驱动实现 (ESP-IDF v5.4 i2c_master API)
 */

#include "tca9554.h"
#include <stdlib.h>
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "tca9554";

struct tca9554_dev {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t                 addr;
    uint8_t                 last_out;   /* 缓存输出值，避免读-修改-写竞态 */
};

static esp_err_t tca9554_reg_read(tca9554_handle_t dev, uint8_t reg, uint8_t *val)
{
    uint8_t buf[1] = { reg };
    return i2c_master_transmit_receive(dev->i2c_dev, buf, 1, val, 1, 100);
}

static esp_err_t tca9554_reg_write(tca9554_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev->i2c_dev, buf, 2, 100);
}

tca9554_handle_t tca9554_new(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
{
    if (i2c_bus == 0) {
        ESP_LOGE(TAG, "i2c_bus is NULL");
        return NULL;
    }

    esp_err_t probe_err = i2c_master_probe(i2c_bus, addr, 100);
    if (probe_err != ESP_OK) {
        ESP_LOGE(TAG, "No I2C response at 0x%02x: %s; check board power and SDA/SCL", addr, esp_err_to_name(probe_err));
        return NULL;
    }

    tca9554_handle_t dev = calloc(1, sizeof(struct tca9554_dev));
    if (dev == NULL) {
        ESP_LOGE(TAG, "calloc failed");
        return NULL;
    }

    dev->addr = addr;

    i2c_device_config_t i2c_conf = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
        .scl_wait_us     = 0,
        .flags           = { 0 },
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &i2c_conf, &dev->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        free(dev);
        return NULL;
    }

    ESP_LOGI(TAG, "TCA9554@0x%02x created", addr);
    return dev;
}

void tca9554_delete(tca9554_handle_t dev)
{
    if (dev == NULL) return;
    if (dev->i2c_dev) {
        i2c_master_bus_rm_device(dev->i2c_dev);
    }
    free(dev);
}

esp_err_t tca9554_set_direction(tca9554_handle_t dev, uint8_t mask)
{
    return tca9554_reg_write(dev, TCA9554_REG_CONFIG, mask);
}

esp_err_t tca9554_read_input(tca9554_handle_t dev, uint8_t *val)
{
    return tca9554_reg_read(dev, TCA9554_REG_INPUT_PORT, val);
}

esp_err_t tca9554_write_output(tca9554_handle_t dev, uint8_t mask, uint8_t val)
{
    uint8_t new_out = (dev->last_out & ~mask) | (val & mask);
    esp_err_t ret = tca9554_reg_write(dev, TCA9554_REG_OUTPUT_PORT, new_out);
    if (ret == ESP_OK) {
        dev->last_out = new_out;
    }
    return ret;
}

esp_err_t tca9554_set_output(tca9554_handle_t dev, uint8_t value)
{
    esp_err_t ret = tca9554_reg_write(dev, TCA9554_REG_OUTPUT_PORT, value);
    if (ret == ESP_OK) {
        dev->last_out = value;
    }
    return ret;
}

esp_err_t tca9554_read_output(tca9554_handle_t dev, uint8_t *val)
{
    return tca9554_reg_read(dev, TCA9554_REG_OUTPUT_PORT, val);
}

esp_err_t tca9554_init_defaults(tca9554_handle_t dev, uint8_t dir, uint8_t out)
{
    /* Set the output latch before enabling outputs to avoid a power glitch. */
    esp_err_t ret = tca9554_set_output(dev, out);
    if (ret != ESP_OK) return ret;
    ret = tca9554_reg_write(dev, TCA9554_REG_POLARITY_INV, 0x00);
    if (ret != ESP_OK) return ret;
    return tca9554_reg_write(dev, TCA9554_REG_CONFIG, dir);
}