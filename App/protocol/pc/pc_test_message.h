/* PC测试业务的第一组JSON结构：算法下发与芯片测试结果上报。 */
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
} pc_test_json_result_t;

/*
 * 字符串指针直接引用cJSON树，调用者必须在使用完view后再cJSON_Delete(root)。
 * algorithmIndex只约束为uint32整数，暂不擅自限定算法编号范围。
 */
typedef struct {
    const char *algorithm_name;  /* 借用root内字符串，不能单独释放。 */
    uint32_t algorithm_index;    /* PC提供的算法编号，当前只约束为u32。 */
} pc_test_start_view_t;

/* MCU准备上报给PC的一条芯片测试结果；本结构不拥有字符串，组包时复制。 */
typedef struct {
    const char *site_id;       /* 辅助函数接受空串；业务有效性尚待冻结。 */
    uint32_t mirror_index;     /* 镜像/测试位编号，业务值域待冻结。 */
    int32_t test_status;       /* 测试状态码，业务枚举待冻结。 */
    const char *result_desc;   /* 结果描述，UTF-8字符串；NULL或空串均上报""。 */
} pc_chip_test_result_t;

/**
 * @brief 提取START_TEST的算法名称与索引，允许额外字段。
 * @param[in] root 非NULL JSON对象；字段名称区分大小写。
 * @param[out] view 非NULL输出视图，失败不修改。
 * @return PC_TEST_JSON_OK成功；其他值区分参数、缺字段、类型及整数范围错误。
 * @note algorithmName须为字符串，algorithmIndex须能无损表示为uint32_t。
 *       名称借用root中的字符串，root须保持存活；此函数不启动RK测试。
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

#ifdef __cplusplus
}
#endif

#endif /* PC_TEST_MESSAGE_H */
