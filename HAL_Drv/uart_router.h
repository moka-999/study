/*
 * UART字节路由接口：不承载TCP转发，不解释RK/PC协议。
 * B1槽0固定UART4协议，槽1为USART2双向调试，槽2为USART1本地控制台。
 */
#ifndef UART_ROUTER_H
#define UART_ROUTER_H

#include "hal_if.h"

/*
 * 接收块容量1600字节，覆盖RK最大帧1040字节和SerialNet最大COBS线帧1540字节。
 * SerialNet停止等待，一帧结束后才回复；容量须大于最大线帧，避免帧中NORMAL DMA满块停收。
 * 状态机仍支持分片；此容量不承诺无间隔任意长字节流可靠传输。
 * DMA RX缓冲位于.uart_dma段，不能放DTCM。容量不代表一次回调必为完整帧。
 */
#define UART_RX_BUF_SIZE 1600U

/* 已丢失字节连续性；消费者必须先重置半帧状态，再继续poll，不能按字节数使用。 */
#define UART_RX_GAP (-1)

enum {
    UART_PORT_RK_PROTOCOL = 0,
    UART_PORT_RK_DEBUG = 1,
    UART_PORT_CONSOLE = 2,
    UART_PORT_COUNT = 3
};

typedef struct {
    uint32_t queue_overflows;  /* ISR投队列失败次数。 */
    uint32_t dropped_blocks;   /* 已知丢弃块数，含断流后清队列；不是物理丢失字节数。 */
    uint32_t errors;           /* HAL错误回调次数。 */
    uint32_t last_hal_error;   /* 最近一次HAL UART错误位图。 */
    uint32_t invalid_events;   /* 非HT事件的长度非法次数。 */
    uint32_t restart_failures; /* 首次/回调/恢复后启动RX失败次数。 */
    uint32_t recovery_failures;/* 任务停止RX失败次数。 */
    uint32_t recoveries;       /* 任务停止并重新启动RX成功次数。 */
    uint8_t recovery_pending; /* 1等待任务恢复；不表示线缆/对端已在线。 */
} uart_rx_diagnostics_t;

/**
 * @brief 调度前为逻辑UART槽注册接口并创建接收队列。
 * @param[in] id UART_PORT_*槽编号，0=协议、1=RK日志、2=本地控制台。
 * @param[in] interface 借用接口，须含start_rx/send并覆盖槽位使用期。
 * @note 无效参数、重复注册或分配失败直接返回；无状态码，失败槽不可用。
 *       每槽只注册一次；注册不启动物理接收。
 */
void uart_register(int id, uart_if_t *interface);

/**
 * @brief 调度后创建TX互斥锁、挂接HAL TxISR并启动已注册槽接收。
 * @note 调用方须先初始化外设；失败槽记录恢复请求，须由唯一接收任务周期poll。
 *       返回不表示所有DMA已成功启动；通过诊断接口查询。TX锁失败拒绝发送。
 */
void uart_start_all(void);

/**
 * @brief 在任务上下文驱动RX恢复，并无队列等待地取走一个接收块。
 * @param[in] id 已注册的UART_PORT_*槽编号。
 * @param[out] buffer 非NULL调用方缓冲，至少UART_RX_BUF_SIZE字节。
 * @param[in] capacity 缓冲实际字节容量；小于要求时不出队。
 * @return >0实际字节数；0无数据/参数无效；UART_RX_GAP表示断流且已丢弃旧队列。
 *         返回<=0时输出不变；GAP时必须重置组帧状态并继续poll。
 * @note 每槽只允许一个任务消费，须周期调用以驱动100ms间隔的RX恢复。
 *       出队不等待，但恢复可等待HAL Abort自身超时；不得ISR调用。
 *       一个接收块不保证是一帧，队列满/异常时不保证可靠传输。
 */
int uart_poll_rx(int id, uint8_t *buffer, uint16_t capacity);

/* 唯一接收任务等待下一块数据，最长timeout_ms毫秒；已排队或有断流标记时立即返回。
 * 只等待、不出队或处理恢复，醒来后仍须uart_poll_rx；不能在ISR或临界区调用。
 * 空闲超时用于推进电源/改速/恢复，不表示收到数据；timeout_ms=0只检查不等待。 */
void uart_wait_rx(int id, uint32_t timeout_ms);

/** 任务上下文复制本次开机累计诊断：1成功，0参数/槽无效且输出不变；计数饱和。 */
int uart_get_rx_diagnostics(int id, uart_rx_diagnostics_t *out);

/**
 * @brief 在任务上下文将载荷交给已注册UART发送接口。
 * @param[in] id UART_PORT_RK_PROTOCOL、UART_PORT_RK_DEBUG或UART_PORT_CONSOLE。
 * @param[in] buffer 非NULL，至少可读length字节，当前STM32实现复制入TX环。
 * @param[in] length 非0字节数，当前TX环最大可接纳2047字节。
 * @return IF_OK已复制入TX环；IF_ERR参数/槽无效、锁失败或环空间不足。
 * @note UART4取锁最多100ms；USART1/USART2取锁0等待。失败不入队部分载荷。IF_OK不代表线上
 *       发完或RK收到；调用方须决定失败重试/丢弃策略，禁止ISR调用。
 */
int uart_send(int id, uint8_t *buffer, uint16_t length);

/* 与uart_send相同的整帧入队合同，另返回非0票据。每端口仅一帧可在途跟踪，
 * 已完成票据保留到下一次跟踪发送；失败不写ticket，不改变已排队字节。 */
int uart_send_tracked(int id, uint8_t *buffer, uint16_t length, uint32_t *ticket);
/* 1本帧最后停止位已发出/0在途/-1参数或票据失效；仅1时输出HAL毫秒时间戳。
 * 不证明RK收到或执行；任务上下文调用，不等待、不清除完成事件。 */
int uart_tx_status(int id, uint32_t ticket, uint32_t *completed_ms);

/* 只读发送空闲证明：TX环空且最后停止位已发出。 */
int uart_tx_idle(int id);
/* 唯一RK消费任务在已协商的静默窗口调用；清旧RX，返回IF_OK/IF_ERR。 */
int uart_set_baud(int id, uint32_t baud);

#endif /* UART_ROUTER_H */
