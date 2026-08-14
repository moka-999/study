/**
 ********************************************************************
 * @file    myiic.c
 * @brief   软件模拟 I2C 驱动实现
 * @note    用 GPIO 模拟 I2C 总线时序，SCL=PH4(推挽), SDA=PH5(开漏)
 *          速率约 250KHz（2us 延时）
 * @source  正点原子 ALIENTEK
 ********************************************************************
 */

#include "myiic.h"

/**
 * @brief   初始化 I2C IO 口
 * @note    SCL 推挽输出，SDA 开漏输出（开漏可实现线与，同时支持输入读取）
 */
void iic_init(void)
{
    GPIO_InitTypeDef gpio_init_struct;

    IIC_SCL_GPIO_CLK_ENABLE();
    IIC_SDA_GPIO_CLK_ENABLE();

    /* SCL: 推挽输出 + 上拉 */
    gpio_init_struct.Pin   = IIC_SCL_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(IIC_SCL_GPIO_PORT, &gpio_init_struct);

    /* SDA: 开漏输出 + 上拉（开漏输出=1时释放总线，可读取外部电平） */
    gpio_init_struct.Pin   = IIC_SDA_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_OUTPUT_OD;
    HAL_GPIO_Init(IIC_SDA_GPIO_PORT, &gpio_init_struct);

    iic_stop();     /* 停止信号，释放总线 */
}

/**
 * @brief   I2C 延时（约 2us，对应 250KHz 速率）
 */
static void iic_delay(void)
{
    volatile uint32_t n = 80;
    while (n--) { __NOP(); }
}

/**
 * @brief   发送 I2C 起始信号
 * @note    SCL 高电平期间，SDA 从高变低
 */
void iic_start(void)
{
    IIC_SDA(1);
    IIC_SCL(1);
    iic_delay();
    IIC_SDA(0);         /* START: SCL=1时 SDA 高→低 */
    iic_delay();
    IIC_SCL(0);         /* 钳住总线，准备收发数据 */
    iic_delay();
}

/**
 * @brief   发送 I2C 停止信号
 * @note    SCL 高电平期间，SDA 从低变高
 */
void iic_stop(void)
{
    IIC_SDA(0);
    iic_delay();
    IIC_SCL(1);
    iic_delay();
    IIC_SDA(1);         /* STOP: SCL=1时 SDA 低→高，释放总线 */
    iic_delay();
}

/**
 * @brief   等待从机 ACK 应答
 * @retval  0:收到ACK  1:超时无应答
 */
uint8_t iic_wait_ack(void)
{
    uint8_t waittime = 0;
    uint8_t rack     = 0;

    IIC_SDA(1);                         /* 释放 SDA，等待从机拉低 */
    iic_delay();
    IIC_SCL(1);                         /* SCL=1，从机发出 ACK */
    iic_delay();

    while (IIC_READ_SDA)                /* 等待 SDA 被从机拉低 */
    {
        waittime++;
        if (waittime > 250)             /* 超时，无应答 */
        {
            iic_stop();
            rack = 1;
            break;
        }
    }

    IIC_SCL(0);                         /* SCL=0，结束 ACK 检测 */
    iic_delay();
    return rack;
}

/**
 * @brief   主机发送 ACK 应答
 */
void iic_ack(void)
{
    IIC_SDA(0);         /* SDA=0 表示 ACK */
    iic_delay();
    IIC_SCL(1);         /* 产生一个时钟脉冲 */
    iic_delay();
    IIC_SCL(0);
    iic_delay();
    IIC_SDA(1);         /* 释放 SDA */
    iic_delay();
}

/**
 * @brief   主机发送 NACK 非应答
 */
void iic_nack(void)
{
    IIC_SDA(1);         /* SDA=1 表示 NACK */
    iic_delay();
    IIC_SCL(1);
    iic_delay();
    IIC_SCL(0);
    iic_delay();
}

/**
 * @brief   I2C 发送一个字节（高位在前）
 * @param   data: 要发送的数据
 */
void iic_send_byte(uint8_t data)
{
    uint8_t t;

    for (t = 0; t < 8; t++)
    {
        IIC_SDA((data & 0x80) >> 7);    /* 取最高位发送 */
        iic_delay();
        IIC_SCL(1);                     /* SCL上升沿，从机采样 */
        iic_delay();
        IIC_SCL(0);
        data <<= 1;                     /* 左移，准备下一位 */
    }
    IIC_SDA(1);                         /* 发送完成，释放 SDA */
}

/**
 * @brief   I2C 读取一个字节（高位在前）
 * @param   ack: 1=读完发ACK, 0=读完发NACK
 * @retval  读取到的数据
 */
uint8_t iic_read_byte(uint8_t ack)
{
    uint8_t i, receive = 0;

    for (i = 0; i < 8; i++)
    {
        receive <<= 1;                  /* 左移，腾出最低位 */
        IIC_SCL(1);
        iic_delay();

        if (IIC_READ_SDA)               /* 采样 SDA */
        {
            receive++;
        }

        IIC_SCL(0);
        iic_delay();
    }

    if (!ack)
        iic_nack();
    else
        iic_ack();

    return receive;
}
