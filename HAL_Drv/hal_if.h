/*
 * ===================================================================
 * 外设抽象接口 — 驱动与具体总线实现之间的函数表
 * ===================================================================
 *
 * 设计目标:
 *   上层应用只依赖本文件的接口类型, 不直接调用 HAL 库函数。
 *   具体总线实例由board_interfaces.h声明，*_hw.c负责HAL绑定。
 *   移植仍需核对引脚、时钟、片选和并发策略，接口不保证零改动。
 *
 * 使用方式:
 *   #include "board_interfaces.h"
 *   i2c1_if.read_reg(&i2c1_if, 0x50, 0x00, &val);
 *
 * 层次关系:
 *   任务/组合层 → BSP器件驱动 → hal_if.h → *_hw.c
 *   GPIO、软件I2C等板级实现仍可能直接调用STM32 HAL。
 *
 * 通用约束：除注明UART内部复制外，缓冲由调用方持有至同步调用结束。
 * I2C/SPI/Flash不内置跨任务事务锁，也不允许从ISR调用；共享总线时由
 * 业务所有者串行访问。IF_OK只表示本次操作成功，不表示业务已完成。
 * ===================================================================
 */
#ifndef HAL_IF_H
#define HAL_IF_H

#include <stdint.h>

/* ── 通用返回码 ── */
#define IF_OK       0
#define IF_ERR     -1
#define IF_TIMEOUT -2

/*
 * ── I2C 总线接口 ────────────────────────────────────────────────
 * dev为未左移的7位地址，由STM32适配层左移1位；不支持10位地址。
 * reg为8位寄存器地址，len以字节计。read_bytes带寄存器地址；
 * write_bytes发送原始载荷；write_cmd是Send Byte事务。
 * B1硬件I2C同步事务超时100ms，在线探测为3次/10ms。
 * handle: HAL I2C_HandleTypeDef* (或软件 I2C 自定义句柄)
 */
typedef struct i2c_if {
    void   *handle;
    int   (*read_reg) (struct i2c_if *bus, uint8_t dev, uint8_t reg, uint8_t *val);
    int   (*write_reg)(struct i2c_if *bus, uint8_t dev, uint8_t reg, uint8_t  val);
    int   (*read_bytes)(struct i2c_if *bus, uint8_t dev, uint8_t reg,
                        uint8_t *buf, uint16_t len);
    int   (*write_bytes)(struct i2c_if *bus, uint8_t dev,
                         const uint8_t *buf, uint16_t len);
    int   (*write_cmd)(struct i2c_if *bus, uint8_t dev, uint8_t cmd);
    int   (*is_ready)  (struct i2c_if *bus, uint8_t dev);
} i2c_if_t;

/*
 * ── SPI 总线接口 ────────────────────────────────────────────────
 * 全双工传输 + 半双工发送；len以字节计，调用期间缓冲必须有效。
 * 片选由器件驱动控制，接口不自动切NSS；B1 SPI4同步超时100ms。
 */
typedef struct spi_if {
    void   *handle;
    int   (*transfer)(struct spi_if *bus, uint8_t *tx, uint8_t *rx, uint16_t len);
    int   (*send)    (struct spi_if *bus, uint8_t *data, uint16_t len);
} spi_if_t;

/*
 * ── UART 串口接口 ───────────────────────────────────────────────
 * send复制整个载荷到内部环形缓冲，不等待物理发送完成；成功后可复用data。
 * 互斥锁竞争最多等待100ms，因此不是完全非阻塞，也不能从ISR调用。
 * start_rx借用DMA缓冲，缓冲需保持有效且位于DMA可访问的SRAM。
 */
typedef struct uart_if {
    void   *handle;
    int   (*send)    (struct uart_if *port, uint8_t *data, uint16_t len);
    int   (*start_rx)(struct uart_if *port, uint8_t *buf, uint16_t size);
} uart_if_t;

/*
 * ── PWM 接口 ────────────────────────────────────────────────────
 * 0~100% 占空比, 底层硬件自动分频/计算
 */
typedef struct pwm_if {
    void   *handle;
    void  (*set_duty)(struct pwm_if *pwm, uint8_t percent);
} pwm_if_t;

/*
 * ── Flash 接口 ────────────────────────────────────────────────────
 * W25Q256 NOR Flash，地址和len单位为字节；info的total_kb单位为KiB。
 * write允许任意范围RMW；erase向外扩展到完整扇区，调用方须先确认边界。
 * 共享扇区缓冲不可重入；同步操作可能包含擦除/编程等待。
 */
typedef struct flash_if {
    void   *handle;
    int   (*init)(struct flash_if *f);
    int   (*read)(struct flash_if *f, uint32_t addr, uint8_t *buf, uint32_t len);
    int   (*write)(struct flash_if *f, uint32_t addr, const uint8_t *buf, uint32_t len);
    int   (*erase)(struct flash_if *f, uint32_t addr, uint32_t len);
    void  (*info)(struct flash_if *f, uint32_t *total_kb, uint32_t *sector, uint32_t *page);
} flash_if_t;

#endif /* HAL_IF_H */
