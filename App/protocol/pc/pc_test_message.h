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

/* MCU准备上报给PC的一条芯片测试结果；本结构不拥有site_id字符串。 */
typedef struct {
    const char *site_id;       /* 辅助函数接受空串；业务有效性尚待冻结。 */
    uint32_t mirror_index;     /* 镜像/测试位编号，业务值域待冻结。 */
    int32_t test_status;       /* 测试状态码，业务枚举待冻结。 */
} pc_chip_test_result_t;

/* 解析CMD_START_TEST包体中两个必填字段；允许后续增加不冲突的扩展字段。 */
pc_test_json_result_t pc_test_start_parse(const cJSON *root,
                                          pc_test_start_view_t *view);

/*
 * 创建CMD_TEST_RESULT_REPORT包体树。成功后调用者负责cJSON_Delete()；
 * siteId允许空字符串，mirrorIndex/testStatus语义和值域等待测试协议评审。
 */
cJSON *pc_test_result_create(const char *algorithm_name,
                             uint32_t algorithm_index,
                             const pc_chip_test_result_t *results,
                             size_t result_count);

#ifdef __cplusplus
}
#endif

#endif /* PC_TEST_MESSAGE_H */
