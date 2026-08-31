/*
 * B1硬件I2C适配
 *
 * I2C1和I2C2由STM32硬件控制器实现。原理图中的“逻辑I2C4”
 * 使用PH4/PH5软件开漏GPIO，也在本文件内实现同一i2c_if_t接口。
 */
#include "board_interfaces.h"
#include "main.h"

extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;

#define I2C_TRANSFER_TIMEOUT_MS 100U

/* 硬件/软件总线都只接受未左移的7位地址；无效请求不能触碰HAL或GPIO。 */
static int bus_is_valid(const i2c_if_t *bus, uint8_t device)
{
    return bus != NULL && bus->handle != NULL && device <= 0x7FU;
}

static int i2c_read_reg(i2c_if_t *bus, uint8_t device, uint8_t reg, uint8_t *value)
{
    if (!bus_is_valid(bus, device) || !value) return IF_ERR;
    return HAL_I2C_Mem_Read((I2C_HandleTypeDef *)bus->handle, device << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, value, 1U,
                            I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_write_reg(i2c_if_t *bus, uint8_t device, uint8_t reg, uint8_t value)
{
    if (!bus_is_valid(bus, device)) return IF_ERR;
    return HAL_I2C_Mem_Write((I2C_HandleTypeDef *)bus->handle, device << 1, reg,
                             I2C_MEMADD_SIZE_8BIT, &value, 1U,
                             I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_read_bytes(i2c_if_t *bus, uint8_t device, uint8_t reg,
                          uint8_t *buffer, uint16_t length)
{
    if (!bus_is_valid(bus, device) || !buffer || length == 0U) return IF_ERR;
    return HAL_I2C_Mem_Read((I2C_HandleTypeDef *)bus->handle, device << 1, reg,
                            I2C_MEMADD_SIZE_8BIT, buffer, length,
                            I2C_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

static int i2c_write_bytes(i2c_if_t *bus, uint8_t device,
                           const uint8_t *buffer, uint16_t length)
{
    if (!bus_is_valid(bus, device) || !buffer || length == 0U) return IF_ERR;
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
    if (!bus_is_valid(bus, device)) return IF_ERR;
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

/*
 * PH4/PH5由外部4.7kΩ上拉。GPIO写1表示释放总线，写0表示主机拉低；
 * 禁止切换成推挽输出，否则从机应答时会产生电气冲突。
 * 时序延时是首板可标定参数，真板用逻辑分析仪确认后只调整此常量。
 */
#define SOFT_I2C_DELAY_CYCLES  120U
#define SOFT_I2C_STRETCH_LIMIT 1000U

static void soft_i2c_delay(void)
{
    for (volatile uint32_t cycle = 0U; cycle < SOFT_I2C_DELAY_CYCLES; cycle++)
        __NOP();
}

static void soft_i2c_sda(uint8_t release)
{
    HAL_GPIO_WritePin(B1_I2C4_SDA_PORT, B1_I2C4_SDA_PIN,
                      release ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void soft_i2c_scl_low(void)
{
    HAL_GPIO_WritePin(B1_I2C4_SCL_PORT, B1_I2C4_SCL_PIN, GPIO_PIN_RESET);
}

/* 释放SCL后读回实际电平，同时兼容从机拉长时钟。 */
static int soft_i2c_scl_high(void)
{
    HAL_GPIO_WritePin(B1_I2C4_SCL_PORT, B1_I2C4_SCL_PIN, GPIO_PIN_SET);
    for (uint32_t wait = 0U; wait < SOFT_I2C_STRETCH_LIMIT; wait++) {
        if (HAL_GPIO_ReadPin(B1_I2C4_SCL_PORT, B1_I2C4_SCL_PIN) == GPIO_PIN_SET)
            return IF_OK;
    }
    return IF_TIMEOUT;
}

static void soft_i2c_stop(void)
{
    soft_i2c_sda(0U);
    soft_i2c_delay();
    (void)soft_i2c_scl_high();
    soft_i2c_delay();
    soft_i2c_sda(1U);
    soft_i2c_delay();
}

static int soft_i2c_start(void)
{
    soft_i2c_sda(1U);
    if (soft_i2c_scl_high() != IF_OK) return IF_TIMEOUT;
    soft_i2c_delay();

    /*
     * SDA被异常占用时先补9个时钟并生成STOP，让中途复位的从机退出残留字节。
     * 恢复后仍为低则拒绝交易，不会无限循环卡死FreeRTOS任务。
     */
    if (HAL_GPIO_ReadPin(B1_I2C4_SDA_PORT, B1_I2C4_SDA_PIN) == GPIO_PIN_RESET) {
        for (uint8_t pulse = 0U; pulse < 9U; pulse++) {
            soft_i2c_scl_low();
            soft_i2c_delay();
            if (soft_i2c_scl_high() != IF_OK) return IF_TIMEOUT;
            soft_i2c_delay();
        }
        soft_i2c_stop();
        if (HAL_GPIO_ReadPin(B1_I2C4_SDA_PORT, B1_I2C4_SDA_PIN) == GPIO_PIN_RESET)
            return IF_ERR;
    }

    soft_i2c_sda(0U);
    soft_i2c_delay();
    soft_i2c_scl_low();
    soft_i2c_delay();
    return IF_OK;
}

static int soft_i2c_write_byte(uint8_t value)
{
    for (uint8_t bit = 0U; bit < 8U; bit++) {
        soft_i2c_sda((value & 0x80U) != 0U);
        soft_i2c_delay();
        if (soft_i2c_scl_high() != IF_OK) return IF_TIMEOUT;
        soft_i2c_delay();
        soft_i2c_scl_low();
        value <<= 1;
    }

    soft_i2c_sda(1U);
    soft_i2c_delay();
    if (soft_i2c_scl_high() != IF_OK) return IF_TIMEOUT;
    soft_i2c_delay();
    int ack = HAL_GPIO_ReadPin(B1_I2C4_SDA_PORT, B1_I2C4_SDA_PIN) == GPIO_PIN_RESET;
    soft_i2c_scl_low();
    soft_i2c_delay();
    return ack ? IF_OK : IF_ERR;
}

static int soft_i2c_read_byte(uint8_t *value, uint8_t acknowledge)
{
    if (!value) return IF_ERR;
    *value = 0U;
    soft_i2c_sda(1U);
    for (uint8_t bit = 0U; bit < 8U; bit++) {
        *value <<= 1;
        if (soft_i2c_scl_high() != IF_OK) return IF_TIMEOUT;
        soft_i2c_delay();
        if (HAL_GPIO_ReadPin(B1_I2C4_SDA_PORT, B1_I2C4_SDA_PIN) == GPIO_PIN_SET)
            *value |= 1U;
        soft_i2c_scl_low();
        soft_i2c_delay();
    }

    soft_i2c_sda(acknowledge ? 0U : 1U);
    soft_i2c_delay();
    if (soft_i2c_scl_high() != IF_OK) return IF_TIMEOUT;
    soft_i2c_delay();
    soft_i2c_scl_low();
    soft_i2c_sda(1U);
    return IF_OK;
}

static int soft_i2c_write_reg(i2c_if_t *bus, uint8_t device,
                              uint8_t reg, uint8_t value)
{
    int result = IF_ERR;
    if (!bus_is_valid(bus, device)) return IF_ERR;
    if (soft_i2c_start() != IF_OK) return IF_ERR;
    if (soft_i2c_write_byte((uint8_t)(device << 1)) != IF_OK) goto done;
    if (soft_i2c_write_byte(reg) != IF_OK) goto done;
    if (soft_i2c_write_byte(value) != IF_OK) goto done;
    result = IF_OK;
done:
    soft_i2c_stop();
    return result;
}

static int soft_i2c_read_bytes(i2c_if_t *bus, uint8_t device, uint8_t reg,
                               uint8_t *buffer, uint16_t length)
{
    int result = IF_ERR;
    if (!bus_is_valid(bus, device)) return IF_ERR;
    if (!buffer || length == 0U || soft_i2c_start() != IF_OK) return IF_ERR;
    if (soft_i2c_write_byte((uint8_t)(device << 1)) != IF_OK) goto done;
    if (soft_i2c_write_byte(reg) != IF_OK) goto done;
    if (soft_i2c_start() != IF_OK) goto done;
    if (soft_i2c_write_byte((uint8_t)((device << 1) | 1U)) != IF_OK) goto done;
    for (uint16_t index = 0U; index < length; index++) {
        if (soft_i2c_read_byte(&buffer[index], index + 1U < length) != IF_OK)
            goto done;
    }
    result = IF_OK;
done:
    soft_i2c_stop();
    return result;
}

static int soft_i2c_read_reg(i2c_if_t *bus, uint8_t device,
                             uint8_t reg, uint8_t *value)
{
    return soft_i2c_read_bytes(bus, device, reg, value, 1U);
}

static int soft_i2c_write_bytes(i2c_if_t *bus, uint8_t device,
                                const uint8_t *buffer, uint16_t length)
{
    int result = IF_ERR;
    if (!bus_is_valid(bus, device)) return IF_ERR;
    if (!buffer || length == 0U || soft_i2c_start() != IF_OK) return IF_ERR;
    if (soft_i2c_write_byte((uint8_t)(device << 1)) != IF_OK) goto done;
    for (uint16_t index = 0U; index < length; index++) {
        if (soft_i2c_write_byte(buffer[index]) != IF_OK) goto done;
    }
    result = IF_OK;
done:
    soft_i2c_stop();
    return result;
}

static int soft_i2c_write_command(i2c_if_t *bus, uint8_t device, uint8_t command)
{
    return soft_i2c_write_bytes(bus, device, &command, 1U);
}

static int soft_i2c_is_ready(i2c_if_t *bus, uint8_t device)
{
    if (!bus_is_valid(bus, device)) return IF_ERR;
    if (soft_i2c_start() != IF_OK) return IF_ERR;
    int result = soft_i2c_write_byte((uint8_t)(device << 1));
    soft_i2c_stop();
    return result;
}

/*
 * ponytail: 当前软件总线不含跨任务锁，调用方必须串行访问完整器件事务；
 * 后续若多个FreeRTOS任务并发访问PH4/PH5，再在器件事务边界加总线互斥锁。
 */
i2c_if_t i2c4_if = {
    .handle = B1_I2C4_SCL_PORT,
    .read_reg = soft_i2c_read_reg,
    .write_reg = soft_i2c_write_reg,
    .read_bytes = soft_i2c_read_bytes,
    .write_bytes = soft_i2c_write_bytes,
    .write_cmd = soft_i2c_write_command,
    .is_ready = soft_i2c_is_ready,
};
