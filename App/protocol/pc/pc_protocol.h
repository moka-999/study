/*
 * PC-MCU 固定头定义、响应组帧与JSON入口。
 *
 * TCP字节流解析状态已独立到pc_state_machine.h；本模块不定义CommandID
 * 业务含义，也不参与MCU-RK的0x5A/0xA5 TLV协议。
 * MCU发送头使用连接内独立u32流水号，ServiceID固定0；接收PC Seq只用于
 * 诊断，不回显、不参与RK序号或业务关联。
 */
#ifndef PC_PROTOCOL_H
#define PC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* 帧状态机只需要不透明JSON指针，不能因此传递引入整个第三方解析器头。 */
typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

#define PC_PROTOCOL_HEADER_SIZE   18U
#define PC_PROTOCOL_VERSION       1U
#define PC_PROTOCOL_FLAG_JSON     8U

/*
 * PC工程师确认的MCU完整PDU上限，包含18字节头部。Length字段本身仍为u32，
 * 但超过4KiB时必须拒绝该候选头，并在有界搜索中寻找后续正常头；不得按超限
 * Length收正文或分配内存，避免占满FreeRTOS堆和任务栈。
 */
#define PC_PROTOCOL_MAX_PDU       4096U
#define PC_PROTOCOL_MAX_JSON_SIZE (PC_PROTOCOL_MAX_PDU - PC_PROTOCOL_HEADER_SIZE)

/*
 * 已解析的PC帧视图，不拥有任何动态内存。
 * 除json/json_length外的字段均由18字节头按网络大端序转换为MCU本机整数。
 */
typedef struct {
    uint32_t length;       /* 完整PDU字节数：18字节头+JSON正文。 */
    uint16_t version;      /* 当前必须为PC_PROTOCOL_VERSION。 */
    uint16_t flag;         /* 当前必须为PC_PROTOCOL_FLAG_JSON。 */
    uint16_t service_id;   /* 初版固定为0；非0帧由状态机拒绝。 */
    uint16_t command_id;   /* 当前PDU的请求或响应命令号。 */
    uint32_t sequence;     /* PC自身发送流水号；接收不改变MCU发送计数。 */
    uint16_t reserved;     /* 预留字段；当前发送响应时固定写0。 */
    const uint8_t *json;   /* 借用PC状态机内部缓冲中的正文，不需要free。 */
    uint32_t json_length;  /* 正文字节数，不包含额外的'\0'结束符。 */
} pc_protocol_frame_t;

typedef enum {
    PC_JSON_OK = 0,               /* 严格JSON对象校验通过。 */
    PC_JSON_ERROR_ARGUMENT,       /* 空指针、空正文或正文长度越界。 */
    PC_JSON_ERROR_UTF8,           /* 正文不是合法UTF-8字节序列。 */
    PC_JSON_ERROR_SYNTAX,         /* cJSON报告JSON语法错误。 */
    PC_JSON_ERROR_TRAILING_DATA,  /* 根对象后仍存在非空白字符。 */
    PC_JSON_ERROR_ROOT_TYPE,      /* 根节点不是{...}对象。 */
    PC_JSON_ERROR_DUPLICATE_KEY,  /* 任意层对象包含同名字段。 */
} pc_json_result_t;

/**
 * @brief 将JSON正文复制并组装为MCU发送的固定头PDU。
 * @param[in] response_command_id PC协议响应命令号。
 * @param[in] mcu_tx_sequence 本连接MCU发送序号，不是请求Seq或RK Seq。
 * @param[in] json 非NULL正文，至少可读json_length字节，不要求NUL结尾。
 * @param[in] json_length 正文字节数，范围1..PC_PROTOCOL_MAX_JSON_SIZE。
 * @param[out] output 调用方缓冲，至少容纳18 + json_length字节。
 * @param[in] output_capacity output实际容量，单位字节。
 * @param[out] output_length 非NULL；成功返回PDU字节数，失败置0。
 * @return 0成功；-1参数无效或容量不足，此时不写output。
 * @note ServiceID和Reserved固定0；不校验JSON语法，不分配内存或发送数据。
 *       json与output写入区域不得重叠，缓冲均由调用方持有。
 */
int pc_protocol_build_response(uint16_t response_command_id,
                               uint32_t mcu_tx_sequence,
                               const uint8_t *json,
                               uint32_t json_length,
                               uint8_t *output,
                               size_t output_capacity,
                               uint32_t *output_length);

/**
 * @brief 配置cJSON全局内存分配器。
 * @param[in] allocate 分配回调，参数为字节数；STM32使用FreeRTOS堆。
 * @param[in] deallocate 与allocate配对的释放回调。
 * @note 任一回调为NULL时恢复默认malloc/free。须在创建JSON对象和并发任务前
 *       配置，不能在旧分配器创建的对象仍存活时切换。
 */
void pc_json_configure_allocator(void *(*allocate)(size_t),
                                 void (*deallocate)(void *));

/**
 * @brief 严格解析UTF-8 JSON对象，拒绝重复键和尾随非空白数据。
 * @param[in] json 非NULL正文，不要求NUL结尾；拒绝原始或转义的内嵌NUL。
 * @param[in] json_length 正文字节数，范围1..PC_PROTOCOL_MAX_JSON_SIZE。
 * @param[out] root 非NULL；成功取得新对象，调用方用cJSON_Delete释放；失败置NULL。
 * @param[out] error_offset 可为NULL；返回可定位错误的零起始字节偏移，默认0。
 * @return PC_JSON_OK成功；其他pc_json_result_t值区分参数、编码、语法、
 *         尾随数据、根类型和重复键错误；分配失败归入语法错误。
 * @note 尾部仅允许标准JSON空白；递归检查所有子对象。error_offset不是独立
 *       错误标志，根类型和重复键等无法定位的错误也保持0。
 */
pc_json_result_t pc_json_parse_object(const uint8_t *json,
                                      size_t json_length,
                                      cJSON **root,
                                      size_t *error_offset);

#ifdef __cplusplus
}
#endif

#endif /* PC_PROTOCOL_H */
