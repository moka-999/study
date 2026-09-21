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
} pc_test_start_view_t;

/* MCU准备上报给PC的一条芯片测试结果；本结构不拥有site_id字符串。 */
typedef struct {
    const char *site_id;       /* 辅助函数接受空串；业务有效性尚待冻结。 */
    uint32_t mirror_index;     /* 镜像/测试位编号，业务值域待冻结。 */
    int32_t test_status;       /* 测试状态码，业务枚举待冻结。 */
} pc_chip_test_result_t;

/**
 * @brief 单独读取唯一且合法的计划项编号，供其他字段错误时关联失败响应。
 * @param[in] root 非NULL JSON对象；仅检查algorithmIndex本身。
 * @param[out] index 非NULL；成功写入u32编号，失败保持原值。
 * @return PC_TEST_JSON_OK成功；缺失、重复、非有限数或非u32整数返回错误。
 */
pc_test_json_result_t pc_test_start_read_index(const cJSON *root, uint32_t *index);

/**
 * @brief 严格校验START_TEST全部六个根字段和power四字段，纯计算目标电压。
 * @param[in] root 非NULL JSON对象；字段名称区分大小写。
 * @param[out] view 非NULL输出视图，失败不修改。
 * @return PC_TEST_JSON_OK成功；其他值区分参数、缺字段、类型及整数范围错误。
 * @note 拒绝重复、缺失、额外字段，数字必须有限且可无损转换，reboot必须为布尔。
 *       algorithmName只校验字符串类型，不臆造算法表或RK编号。TPS编码范围校验
 *       不能代替DDR安全范围批准；本函数不写硬件、不改变配置、不启动RK测试。
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
 */
cJSON *pc_test_result_create(const char *algorithm_name,
                             uint32_t algorithm_index,
                             const pc_chip_test_result_t *results,
                             size_t result_count);

#ifdef __cplusplus
}
#endif

#endif /* PC_TEST_MESSAGE_H */
