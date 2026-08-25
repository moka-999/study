/*
 * B1硬件I2C适配
 *
 * I2C1和I2C2由STM32硬件控制器实现。原理图中的“逻辑I2C4”
 * 使用PH4/PH5软件开漏GPIO，不属于本文件，后续单独接入。
 */
#include "hal_if.h"
#include "main.h"

extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;

#define I2C_TRANSFER_TIMEOUT_MS 100U

static int i2c_read_reg(i2c_if_t *bus, uint8_t device, uint8_t reg, uint8_t *value)
{
    if (!bus || !bus->handle || !value) return IF_ERR;
    return HAL_I2C_Mem_Read((I2C_HandleTypeDef *)bus->handle, device << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, value, 1U,
                            I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_write_reg(i2c_if_t *bus, uint8_t device, uint8_t reg, uint8_t value)
{
    if (!bus || !bus->handle) return IF_ERR;
    return HAL_I2C_Mem_Write((I2C_HandleTypeDef *)bus->handle, device << 1, reg,
                             I2C_MEMADD_SIZE_8BIT, &value, 1U,
                             I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_read_bytes(i2c_if_t *bus, uint8_t device, uint8_t reg,
                          uint8_t *buffer, uint16_t length)
{
    if (!bus || !bus->handle || !buffer || length == 0U) return IF_ERR;
    return HAL_I2C_Mem_Read((I2C_HandleTypeDef *)bus->handle, device << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, buffer, length,
                            I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_write_bytes(i2c_if_t *bus, uint8_t device,
                           const uint8_t *buffer, uint16_t length)
{
    if (!bus || !bus->handle || !buffer || length == 0U) return IF_ERR;
    return HAL_I2C_Master_Transmit((I2C_HandleTypeDef *)bus->handle, device << 1,
                                   (uint8_t *)buffer, length,
                                   I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_write_command(i2c_if_t *bus, uint8_t device, uint8_t command)
{
    return i2c_write_bytes(bus, device, &command, 1U);
}

static int i2c_is_ready(i2c_if_t *bus, uint8_t device)
{
    if (!bus || !bus->handle) return IF_ERR;
    return HAL_I2C_IsDeviceReady((I2C_HandleTypeDef *)bus->handle, device << 1,
                                 3U, 10U) == HAL_OK ? IF_OK : IF_ERR;
}

#define B1_I2C_INTERFACE(handle_ptr) { \
    .handle = (handle_ptr), .read_reg = i2c_read_reg, .write_reg = i2c_write_reg, \
    .read_bytes = i2c_read_bytes, .write_bytes = i2c_write_bytes, \
    .write_cmd = i2c_write_command, .is_ready = i2c_is_ready \
}

i2c_if_t i2c1_if = B1_I2C_INTERFACE(&hi2c1);
i2c_if_t i2c2_if = B1_I2C_INTERFACE(&hi2c2);
