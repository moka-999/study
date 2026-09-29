/**
 ********************************************************************
 * @file    pcf8574.c
 * @brief   PCF8574 I2C 8位IO扩展芯片驱动实现
 * @source  正点原子 ALIENTEK
 ********************************************************************
 */

#include "pcf8574.h"

/* 缓存的是输出锁存器，不是引脚电平；只有完整ACK成功才允许继续按位修改。 */
static uint8_t output_latch = 0xFFU;
static uint8_t latch_valid;

/* 调用方串行化整笔I2C事务；当前仅在ETH初始化阶段使用。 */
int pcf8574_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    latch_valid = 0U;
    PCF8574_GPIO_CLK_ENABLE();
    gpio.Pin = PCF8574_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(PCF8574_GPIO_PORT, &gpio);
    iic_init();
    return pcf8574_write_byte(0xFFU);
}

int pcf8574_read_byte(void)
{
    iic_start();
    iic_send_byte(PCF8574_ADDR | 1U);
    if (iic_wait_ack() != 0U) {
        iic_stop();
        return -1;
    }
    uint8_t value = iic_read_byte(0U);
    iic_stop();
    return value;
}

int pcf8574_write_byte(uint8_t data)
{
    /* 准双向输入必须写1释放；外部中断拉低不能反写成输出低。 */
    data |= (1U << AP_INT_IO) | (1U << MPU_INT_IO);
    latch_valid = 0U;
    iic_start();
    iic_send_byte(PCF8574_ADDR);
    if (iic_wait_ack() != 0U) {
        iic_stop();
        return -1;
    }
    iic_send_byte(data);
    if (iic_wait_ack() != 0U) {
        iic_stop();
        return -1;
    }
    iic_stop();
    output_latch = data;
    latch_valid = 1U;
    return 0;
}

int pcf8574_write_bit(uint8_t bit, uint8_t state)
{
    if (bit > 7U || state > 1U || !latch_valid ||
        ((bit == AP_INT_IO || bit == MPU_INT_IO) && state == 0U)) return -1;
    uint8_t data = output_latch & (uint8_t)~(1U << bit);
    if (state != 0U) data |= (uint8_t)(1U << bit);
    return pcf8574_write_byte(data);
}

int pcf8574_read_bit(uint8_t bit)
{
    if (bit > 7U) return -1;
    int data = pcf8574_read_byte();
    return data < 0 ? -1 : ((data >> bit) & 1);
}
