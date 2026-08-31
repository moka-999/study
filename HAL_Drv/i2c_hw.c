/*
 * Apollo V2硬件I2C适配：7位地址由本层左移一次，再调用STM32 HAL。
 * 只处理总线事务，不解释TPS/PAC/MCP寄存器或决定器件初始化顺序。
 * 事务同步执行；HAL非成功状态沿用IF_ERR，不改变现有调用方合同。
 */
#include "board_interfaces.h"
#include "i2c.h"

#include <stddef.h>

#define I2C_TRANSFER_TIMEOUT_MS 100U
#define I2C_READY_TIMEOUT_MS     10U
#define I2C_READY_TRIALS          3U

/* 空接口、未配置的I2C2和超过7位范围的地址不能进入HAL。 */
static int bus_is_valid(const i2c_if_t *bus, uint8_t device_address)
{
    return bus != NULL && bus->handle != NULL && device_address <= 0x7FU;
}

static int i2c_read_bytes(i2c_if_t *bus, uint8_t device_address,
                         uint8_t register_address, uint8_t *buffer,
                         uint16_t length)
{
    if (!bus_is_valid(bus, device_address) || buffer == NULL || length == 0U) {
        return IF_ERR;
    }
    return HAL_I2C_Mem_Read(bus->handle, (uint16_t)device_address << 1,
                           register_address, I2C_MEMADD_SIZE_8BIT,
                           buffer, length, I2C_TRANSFER_TIMEOUT_MS) == HAL_OK
               ? IF_OK : IF_ERR;
}

/* 单寄存器读复用批量读，保持参数检查、地址和超时完全一致。 */
static int i2c_read_register(i2c_if_t *bus, uint8_t device_address,
                            uint8_t register_address, uint8_t *value)
{
    return i2c_read_bytes(bus, device_address, register_address, value, 1U);
}

static int i2c_write_register(i2c_if_t *bus, uint8_t device_address,
                             uint8_t register_address, uint8_t value)
{
    if (!bus_is_valid(bus, device_address)) {
        return IF_ERR;
    }
    return HAL_I2C_Mem_Write(bus->handle, (uint16_t)device_address << 1,
                            register_address, I2C_MEMADD_SIZE_8BIT,
                            &value, 1U, I2C_TRANSFER_TIMEOUT_MS) == HAL_OK
               ? IF_OK : IF_ERR;
}

/* 原始写：例如MCP4725 Fast Write，buffer自身已包含完整器件载荷。 */
static int i2c_write_bytes(i2c_if_t *bus, uint8_t device_address,
                          const uint8_t *buffer, uint16_t length)
{
    if (!bus_is_valid(bus, device_address) || buffer == NULL || length == 0U) {
        return IF_ERR;
    }
    /* HAL旧签名未声明const；Master_Transmit不会修改发送缓冲。 */
    return HAL_I2C_Master_Transmit(bus->handle, (uint16_t)device_address << 1,
                                  (uint8_t *)buffer, length,
                                  I2C_TRANSFER_TIMEOUT_MS) == HAL_OK
               ? IF_OK : IF_ERR;
}

/* Send Byte：START→地址+W→command→STOP，没有额外寄存器或数据字节。 */
static int i2c_write_command(i2c_if_t *bus, uint8_t device_address, uint8_t command)
{
    return i2c_write_bytes(bus, device_address, &command, 1U);
}

static int i2c_is_ready(i2c_if_t *bus, uint8_t device_address)
{
    if (!bus_is_valid(bus, device_address)) {
        return IF_ERR;
    }
    return HAL_I2C_IsDeviceReady(bus->handle, (uint16_t)device_address << 1,
                                I2C_READY_TRIALS, I2C_READY_TIMEOUT_MS) == HAL_OK
               ? IF_OK : IF_ERR;
}

/* 具体引脚以本工程.ioc为准；这里只绑定已生成的HAL句柄。 */
i2c_if_t i2c1_if = {
    .handle = &hi2c1,
    .read_reg = i2c_read_register,
    .write_reg = i2c_write_register,
    .read_bytes = i2c_read_bytes,
    .write_bytes = i2c_write_bytes,
    .write_cmd = i2c_write_command,
    .is_ready = i2c_is_ready,
};

/* V2无I2C2；最终B1另行绑定PF0/PF1，不能称为B2总线。 */
i2c_if_t i2c2_if = {
    .handle = NULL,
    .read_reg = i2c_read_register,
    .write_reg = i2c_write_register,
    .read_bytes = i2c_read_bytes,
    .write_bytes = i2c_write_bytes,
    .write_cmd = i2c_write_command,
    .is_ready = i2c_is_ready,
};

i2c_if_t i2c3_if = {
    .handle = &hi2c3,
    .read_reg = i2c_read_register,
    .write_reg = i2c_write_register,
    .read_bytes = i2c_read_bytes,
    .write_bytes = i2c_write_bytes,
    .write_cmd = i2c_write_command,
    .is_ready = i2c_is_ready,
};

/* V2为PF14/PF15硬件I2C4；B1同名接口使用PH4/PH5软件开漏实现。 */
i2c_if_t i2c4_if = {
    .handle = &hi2c4,
    .read_reg = i2c_read_register,
    .write_reg = i2c_write_register,
    .read_bytes = i2c_read_bytes,
    .write_bytes = i2c_write_bytes,
    .write_cmd = i2c_write_command,
    .is_ready = i2c_is_ready,
};
