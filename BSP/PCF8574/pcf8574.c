/**
 ********************************************************************
 * @file    pcf8574.c
 * @brief   PCF8574 I2C 8位IO扩展芯片驱动实现
 * @source  正点原子 ALIENTEK
 ********************************************************************
 */

#include "pcf8574.h"

/**
 * @brief   初始化 PCF8574
 * @note    配置 PB12 为中断输入(上拉)，初始化软件 I2C，
 *          检测 PCF8574 是否在线，默认所有 IO 输出高电平
 * @retval  0:成功  1:失败(无 ACK 应答)
 */
uint8_t pcf8574_init(void)
{
    uint8_t temp = 0;
    GPIO_InitTypeDef gpio_init_struct;

    /* ── PB12 配置为中断输入 ── */
    PCF8574_GPIO_CLK_ENABLE();
    gpio_init_struct.Pin   = PCF8574_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_INPUT;
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(PCF8574_GPIO_PORT, &gpio_init_struct);

    /* ── 初始化软件 I2C ── */
    iic_init();

    /* ── 发送器件地址，检测是否有 ACK 应答 ── */
    iic_start();
    iic_send_byte(PCF8574_ADDR);
    temp = iic_wait_ack();               /* 0=有应答(芯片在线), 1=无应答 */
    iic_stop();

    /* ── 默认所有 IO 输出高电平 ── */
    pcf8574_write_byte(0xFF);

    return temp;
}

/**
 * @brief   读取 PCF8574 全部 8 位 IO 值
 * @retval  8 位 IO 状态
 */
uint8_t pcf8574_read_byte(void)
{
    uint8_t temp = 0;

    iic_start();
    iic_send_byte(PCF8574_ADDR | 0x01);   /* 读模式 */
    iic_wait_ack();
    temp = iic_read_byte(0);
    iic_stop();

    return temp;
}

/**
 * @brief   向 PCF8574 写入全部 8 位 IO 值
 * @param   data: 要写入的数据
 */
void pcf8574_write_byte(uint8_t data)
{
    iic_start();
    iic_send_byte(PCF8574_ADDR | 0x00);   /* 写模式 */
    iic_wait_ack();
    iic_send_byte(data);
    iic_wait_ack();
    iic_stop();
    HAL_Delay(10);
}

/**
 * @brief   设置 PCF8574 某一位 IO 的高低电平
 * @param   bit: IO 编号 (0~7)
 * @param   sta: 0=低电平, 1=高电平
 * @note    采用读-修改-写方式，不影响其他 IO
 */
void pcf8574_write_bit(uint8_t bit, uint8_t sta)
{
    uint8_t data;

    data = pcf8574_read_byte();           /* 先读出当前全部 IO 状态 */
    if (sta == 0)
    {
        data &= ~(1 << bit);              /* 目标位清零 */
    }
    else
    {
        data |= (1 << bit);               /* 目标位置1  */
    }
    pcf8574_write_byte(data);             /* 写回 */
}

/**
 * @brief   读取 PCF8574 某一位 IO 的值
 * @param   bit: IO 编号 (0~7)
 * @retval  0 或 1
 */
uint8_t pcf8574_read_bit(uint8_t bit)
{
    uint8_t data;

    data = pcf8574_read_byte();
    if (data & (1 << bit))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}
