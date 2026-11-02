/*
 * PC-MCU本地命令分发层。
 *
 * 输入是状态机已经收齐的完整帧；先严格解析JSON，再按CommandID查只读命令表，
 * 调用对应处理函数校验方向/字段并生成完整PC响应PDU。
 * 它不调用socket；RK执行由独立测试业务层编排，新增命令添加处理函数和表项。
 */
#ifndef PC_DISPATCH_H
#define PC_DISPATCH_H

#include "pc_protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 容纳双颗DDR结果、最长转义FRU标签和18项共用统计；含18字节头，PDU上限仍为4096。 */
#define PC_DISPATCH_RESPONSE_CAPACITY 1024U

typedef enum {
    PC_DISPATCH_ERROR = -1,          /* 接口参数或内部组包失败，应关闭连接。 */
    PC_DISPATCH_NO_RESPONSE = 0,     /* 合法帧，但当前没有本地同步响应。 */
    PC_DISPATCH_RESPONSE_READY = 1,  /* response中已有可直接发送的完整PDU。 */
} pc_dispatch_result_t;

typedef struct {
    uint8_t data[PC_DISPATCH_RESPONSE_CAPACITY]; /* 18字节头+JSON正文。 */
    uint32_t length;                           /* data中的有效字节数。 */
} pc_dispatch_response_t;

/**
 * @brief 校验JSON并按CommandID执行一条PC本地业务。
 * @param[in] request 非NULL完整帧，头部须先由PC状态机校验；正文借用至返回。
 * @param[in] mcu_tx_sequence 本连接MCU待发送序号，本函数不推进计数。
 * @param[out] response 非NULL响应缓冲；两个指针有效后先将length清0。
 * @return RESPONSE_READY表示完整响应PDU可发送；NO_RESPONSE表示当前无同步
 *         响应；ERROR表示接口参数或内部错误，TCP调用方应关闭连接。
 * @note 严格JSON合法的未知命令返回0x0006/notSupported；业务参数错误用失败
 *       应答表达；完整帧的UTF-8/JSON错误也回invalidArgument，不执行任何业务。
 *       解析树在返回前释放，原始帧仍归调用方。仅成功发送完整PDU
 *       后由TCP任务推进Seq；不调用socket或透传RK帧。任务上下文执行，部分命令
 *       会同步访问Flash/I2C或等待事务锁，返回响应就绪不代表PC已收到。
 */
pc_dispatch_result_t pc_dispatch_handle(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    pc_dispatch_response_t *response);

/** TCP有界等待后调用；无连接也推进电源编排和业务超时，不读写socket或推进Seq。 */
void pc_dispatch_poll(void);
/* 由唯一TCP任务周期调用，领取异步完整PDU；1就绪、0无、-1失败须关闭连接。
 * 使用传入的MCU发送序号，不回显PC/RK序号。成功发送后TCP任务才递增计数。
 */
int pc_dispatch_next_response(uint32_t sequence, pc_dispatch_response_t *response);
/** 当前连接结束时调用，清理连接拥有的未提交业务；不取消已发送给RK的算法。 */
void pc_dispatch_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_DISPATCH_H */
