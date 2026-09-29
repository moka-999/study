/* PC测试业务JSON：完整启动请求的纯校验与芯片测试结果组包。 */
#ifndef PC_TEST_MESSAGE_H
#define PC_TEST_MESSAGE_H

#include "cJSON.h"
#include "pc_command.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_TEST_JSON_OK = 0,                 /* 业务字段校验通过。 */
    PC_TEST_JSON_ERROR_ARGUMENT,         /* root/view等调用参数非法。 */
    PC_TEST_JSON_ERROR_MISSING_FIELD,    /* 缺少协议要求的必填字段。 */
    PC_TEST_JSON_ERROR_FIELD_TYPE,       /* 字段类型错误或数字不是整数。 */
    PC_TEST_JSON_ERROR_INTEGER_RANGE,    /* 整数超出目标C类型范围。 */
    PC_TEST_JSON_ERROR_FIELDS,           /* 字段重复、未知、缺失或枚举值不支持。 */
    PC_TEST_JSON_ERROR_POWER_RANGE,      /* 三轨目标不能用TPS寄存器准确表示。 */
} pc_test_json_result_t;

/*
 * 字符串指针直接引用cJSON树，调用者必须在使用完view后再cJSON_Delete(root)。
 * algorithmIndex为PC测试计划项编号，不是RK算法编号；不自行递增或重排。
 */
typedef struct {
    const char *algorithm_type;  /* functional、slt或parametric；借用root。 */
    const char *algorithm_name;  /* 借用root内字符串，不能单独释放。 */
    uint32_t algorithm_index;    /* PC计划项编号，有限的0..UINT32_MAX整数。 */
    const char *memory_type;     /* LPDDR4或LPDDR4X；借用power中的字符串。 */
    int16_t power_steps[3];      /* VDD1/VDD2/VDDQ相对标称值的20mV有符号档位。 */
    uint16_t target_mv[3];       /* 三路目标mV；可编码不等于DDR安全或已经生效。 */
    uint8_t reboot;              /* JSON布尔值，false=0、true=1。 */
    const char *boot_mode;       /* 已支持的Boot模式名称；不是PXE开关。 */
    uint8_t external_ready;      /* 1=power/bootMode均为null，保持RK现有供电和启动状态。 */
    uint32_t timeout_ms;         /* 最终结果等待毫秒数；0=不限制，非零最多86400000。 */
} pc_test_start_view_t;

/* MCU准备上报给PC的一条芯片测试结果；本结构不拥有字符串，组包时复制。 */
typedef struct {
    const char *site_id;       /* 辅助函数接受空串；业务有效性尚待冻结。 */
    uint32_t mirror_index;     /* B2测试机编号。 */
    int32_t test_status;       /* 测试状态码，业务枚举待冻结。 */
    const char *result_desc;   /* 结果描述，UTF-8字符串；NULL或空串均上报""。 */
} pc_chip_test_result_t;

/**
 * @brief 单独读取唯一且合法的计划项编号，供其他字段错误时关联失败响应。
 * @param[in] root 非NULL JSON对象；仅检查algorithmIndex本身。
 * @param[out] index 非NULL；成功写入u32编号，失败保持原值。
 * @return PC_TEST_JSON_OK成功；缺失、重复、非有限数或非u32整数返回错误。
 */
pc_test_json_result_t pc_test_start_read_index(const cJSON *root, uint32_t *index);

/**
 * @brief 校验START_TEST六个必填字段及可选timeoutMs，不操作任何硬件。
 * @param[in] root 非NULL JSON对象；网络正文须先经pc_json_parse_object拒绝内嵌NUL。
 * @param[out] view 非NULL输出视图，失败不修改。
 * @return PC_TEST_JSON_OK成功；其他值区分参数、缺字段、类型及整数范围错误。
 * @note 拒绝重复、缺失、额外字段，数字必须有限且可无损转换，reboot必须为布尔。
 *       power和bootMode同时为null且reboot=false时，输出external_ready=1，
 *       memory_type/boot_mode为NULL且目标电压为0；0不表示要求输出0V。
 *       timeoutMs省略或为0表示不限制结果等待，非零整数范围为1..86400000毫秒。
 *       电压对象仍严格要求四个字段。TPS可编码不等于DDR安全范围已经批准。
 *       algorithmName只校验字符串类型，具体算法支持情况由命令表决定。
 */
pc_test_json_result_t pc_test_start_parse(const cJSON *root,
                                          pc_test_start_view_t *view);

/**
 * @brief 创建算法结果JSON对象，并复制各字符串到对象树。
 * @param[in] algorithm_name 非NULL、NUL结尾的算法名称。
 * @param[in] algorithm_index 算法索引。
 * @param[in] results 含result_count项的数组；仅在项数为0时可为NULL。
 * @param[in] result_count 结果项数；每项site_id须为非NULL的NUL结尾字符串。
 * @return 新cJSON对象，调用方用cJSON_Delete释放；参数或分配失败返回NULL。
 * @note 不限制尚未冻结的业务编号/状态范围；不检查最终序列化PDU容量。
 *       每项均输出字符串resultDesc；result_desc为NULL时使用空串，否则须为
 *       NUL结尾的UTF-8字符串，并在调用期间保持有效。
 */
cJSON *pc_test_result_create(const char *algorithm_name,
                             uint32_t algorithm_index,
                             const pc_chip_test_result_t *results,
                             size_t result_count);

/**
 * @brief 为一次真实FUNC算法最终结果创建0x0105 JSON，仅输出一条结果。
 * @param[in] algorithm_name 非NULL、NUL结尾的算法名称，复制到结果中。
 * @param[in] algorithm_index PC计划项编号，原样回带。
 * @param[in] site_id 非NULL板卡标志字符串，内容由业务层加载。
 * @param[in] mirror_index B2测试机编号；0可表示尚未分配。
 * @param[in] physical_addr 非NULL颗粒标志；尚未确定映射时传空字符串。
 * @param[in] error_address RK首错地址；0输出PASS(1)及null，否则FAIL(2)及16位十六进制字符串。
 * @param[in] elapsed_ms RK上报的本次算法整体耗时，单位ms；0也是有效值，不用MCU等待时间替代。
 * @return 新cJSON对象，由调用方cJSON_Delete；参数或分配失败返回NULL。
 * @note resultDesc为对象，errorType和measurementData为null，不能用伪造的0代替未知值。
 *       顶层testTimeMs按uint64十进制JSON整数无损输出，不经过double换算。
 */
cJSON *pc_functional_result_create(const char *algorithm_name,
                                    uint32_t algorithm_index,
                                    const char *site_id,
                                    uint32_t mirror_index,
                                    const char *physical_addr,
                                    uint64_t error_address,
                                    uint64_t elapsed_ms);

/* 仅借用统计快照，不依赖采样任务或器件；组包不会推进窗口或增加样本。
 * statistics用于0602，measurementData用于0105唯一结果项，字段沿用既有合同。
 * 无样本的单项写null；返回1成功、0参数/分配失败，失败时调用方删除整个body。
 */
struct idd_stats;
int pc_test_add_statistics(cJSON *body, const struct idd_stats *stats);
int pc_test_add_measurement(cJSON *body, const struct idd_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* PC_TEST_MESSAGE_H */
