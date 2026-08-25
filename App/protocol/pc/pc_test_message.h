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
    PC_TEST_JSON_OK = 0,
    PC_TEST_JSON_ERROR_ARGUMENT,
    PC_TEST_JSON_ERROR_MISSING_FIELD,
    PC_TEST_JSON_ERROR_FIELD_TYPE,
    PC_TEST_JSON_ERROR_INTEGER_RANGE,
} pc_test_json_result_t;

/*
 * 字符串指针直接引用cJSON树，调用者必须在使用完view后再cJSON_Delete(root)。
 * algorithmIndex只约束为uint32整数，暂不擅自限定算法编号范围。
 */
typedef struct {
    const char *algorithm_name;
    uint32_t algorithm_index;
} pc_test_start_view_t;

typedef struct {
    const char *site_id;
    uint32_t mirror_index;
    int32_t test_status;
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
