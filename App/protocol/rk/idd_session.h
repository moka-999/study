/* IDD纯会话：进入/退出组帧、5ms建立等待及5s逐点电流采集。
 * 不访问UART/传感器，不占用全局缓冲，不组装PC JSON，不判断芯片PASS/FAIL。
 * 调用方须串行化全部接口，独占同一RK测试事务，并提供实际发送完成和新采样事件。
 */
#ifndef RK_IDD_SESSION_H
#define RK_IDD_SESSION_H

#include "frame.h"
#include <stdint.h>

/* 用户确认的开发默认值，不是JEDEC规定或已验收的硬件建立时间。 */
#define IDD_SESSION_SETTLE_MS 5U
#define IDD_SESSION_WINDOW_MS 5000U
#define IDD_SESSION_CAPACITY  5120U /* 按1024组/s保存5s，三轨合为一组。 */
#define IDD_SESSION_RAILS     3U
#define IDD_SESSION_VALID_ALL 0x07U

typedef enum {
    IDD_SESSION_IDLE = 0,
    IDD_SESSION_ENTER_READY,
    IDD_SESSION_ENTER_TX,
    IDD_SESSION_SETTLING,
    IDD_SESSION_CAPTURING,
    IDD_SESSION_EXIT_READY,
    IDD_SESSION_EXIT_TX,
    IDD_SESSION_COMPLETE, /* 本地采集结束且退出帧已发完；不表示RK确认或电流合格。 */
    IDD_SESSION_TX_FAULT  /* 发送状态不确定；取走结果后仍须明确掉电才能重新开始。 */
} idd_session_phase_t;

/* 采集质量/传输错误位，可同时出现；不是电流限值判定。 */
enum {
    IDD_SESSION_READ_ERROR = 1U << 0,
    IDD_SESSION_OVERFLOW = 1U << 1,
    IDD_SESSION_SAMPLE_GAP = 1U << 2,
    IDD_SESSION_SAMPLE_ORDER = 1U << 3,
    IDD_SESSION_NO_DATA = 1U << 4,
    IDD_SESSION_CANCELLED = 1U << 5,
    IDD_SESSION_POWER_LOST = 1U << 6,
    IDD_SESSION_TX_ERROR = 1U << 7
};

/* 每点16字节；电流µA按VDD1/VDD2/VDDQ排列，无效轨值为0且valid_mask位为0。
 * offset_ms是物理读取完成时间相对采集起点的0..4999ms，同毫秒允许多个新样本。
 */
typedef struct {
    uint32_t current_ua[IDD_SESSION_RAILS];
    uint16_t offset_ms;
    uint8_t valid_mask;
    uint8_t reserved;
} idd_session_point_t;

typedef struct {
    uint64_t sum_ua;
    uint32_t mean_ua; /* 所保存有效样点的算术均值，向下取整；不是时间加权均值。 */
    uint32_t minimum_ua;
    uint32_t maximum_ua;
    uint32_t valid_count;
    uint32_t failed_count;
} idd_session_metric_t;

typedef struct {
    uint16_t command;             /* 21..36，与进入/退出使用同一编号。 */
    uint32_t flags;
    uint32_t started_ms;          /* 统一uint32_t毫秒时基，允许回绕。 */
    uint32_t collected_ms;        /* 正常5000；提前结束保留实际窗口长度。 */
    uint32_t stored_samples;      /* 含读取失败记录，不把失败点补成有效零电流。 */
    uint32_t missing_samples;     /* 调用方读取流水号的缺口，不是PAC内部丢转换计数。 */
    uint32_t rejected_samples;    /* 重复/逆序事件；不覆盖先前已保存点。 */
    uint32_t boundary_samples;    /* 起止时间不完全落入窗口的读取次数。 */
    uint32_t max_gap_ms;          /* 含窗口首尾，最大无新记录间隔；不是速率验收值。 */
    uint64_t enter_sequence;
    uint64_t exit_sequence;
    idd_session_metric_t current[IDD_SESSION_RAILS];
} idd_session_result_t;

typedef struct {
    idd_session_phase_t phase;
    idd_session_point_t *points; /* 借用缓冲，只在采集中写；冻结统计后解除借用。 */
    uint32_t capacity;
    uint32_t last_sample_sequence;
    uint16_t last_offset_ms;
    uint8_t frozen;
    uint8_t result_taken;
    idd_session_result_t result;
} idd_session_t;

/* session首次必须零初始化。仅IDLE受理；0成功/-1参数或状态错误，拒绝不改旧事务。
 * points由调用方持有，容量1..5120；推荐5120项共81920字节，不能放任务栈。
 * 采样缓冲静态预留 RAM，采样时无需动态分配或清空整块缓冲。
 */
int idd_session_begin(idd_session_t *session, uint16_t command,
                      idd_session_point_t *points, uint32_t capacity);

/* 仅ENTER_READY/EXIT_READY组帧；返回27字节后转对应TX态，0表示无帧/缓冲不足。
 * sequence由RK任务统一分配；失败不推进，不自动重发。组帧不等于发送。
 */
int idd_session_next_frame(idd_session_t *session, uint64_t sequence,
                           uint8_t *wire, uint16_t capacity);
/* 只在本帧最后一字节真正发出后调用，不能用uart_send的入队成功冒充；0成功/-1状态错误。
 * sequence必须匹配该阶段已组帧的Seq，不能使用心跳或上一阶段发送事件。
 * 进入帧完成后等5ms；退出帧完成后结束，不等待RK应答，不接收FUNC结果作为IDD完成。
 */
int idd_session_tx_complete(idd_session_t *session, uint64_t sequence, uint32_t now_ms);
/* 排队失败或发送超时由适配层通知；保守进入TX_FAULT，不自动重发或放行下一IDD。 */
void idd_session_tx_failed(idd_session_t *session, uint64_t sequence, uint32_t now_ms);
/* 非阻塞推进；至少周期调用，超过5s即冻结统计并准备退出，不依赖是否收到RK帧。 */
void idd_session_poll(idd_session_t *session, uint32_t now_ms);
/* 每次实际电流读取只喂一次；sample_sequence按读取尝试递增，允许uint32_t回绕。
 * begin/end为读取起止毫秒；必须整个读取落在5s窗口内。valid_mask低3位逐轨有效。
 * 1保存，0窗口外/重复/未采集/溢出，-1参数错误；无效轨不读取current_ua对应元素。
 * 所有有效新点顺序保存，不降采样、不覆盖；容量耗尽会标记OVERFLOW并提前准备退出。
 */
int idd_session_feed(idd_session_t *session, uint32_t sample_sequence,
                     uint32_t begin_ms, uint32_t end_ms,
                     const uint32_t current_ua[IDD_SESSION_RAILS], uint8_t valid_mask);
/* 内部清理入口：已进入则仍准备退出；发送在途时等发送事件，不静默解除RK占用。 */
void idd_session_cancel(idd_session_t *session, uint32_t now_ms);
/* 仅适配层确认真实掉电后调用；不操作电源，冻结部分统计并解除TX_FAULT。 */
void idd_session_power_off(idd_session_t *session, uint32_t now_ms);
/* 仅COMPLETE/TX_FAULT复制一次结果：1成功/0无结果或空指针。正常事务回IDLE，
 * TX_FAULT保持阻塞直到power_off；统计保留且逐点缓冲已释放借用，无PASS/FAIL字段。
 */
int idd_session_take_result(idd_session_t *session, idd_session_result_t *out);

#endif
