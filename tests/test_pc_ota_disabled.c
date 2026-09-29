/* 普通镜像直接执行正式固件升级分发；不链接OTA后台或硬件，任何误调用都会链接失败。 */
#include "pc_ota.h"
#include "pc_command.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static _Noreturn void exit_test(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}

#define CHECK(condition) do { if (!(condition)) exit_test(__LINE__); } while (0)

/* 仅供QEMU中的真实cJSON/newlib分配；不使用生产FreeRTOS堆。 */
void *_sbrk(ptrdiff_t increment)
{
    static uint64_t heap[8192];
    static ptrdiff_t used;
    if (increment < -used || increment > (ptrdiff_t)sizeof(heap) - used) return (void *)-1;
    void *result = (uint8_t *)heap + used;
    used += increment;
    return result;
}

void test_abort(void) { exit_test(1U); }

void test_main(void)
{
    static const char *const names[] = {
        "rk_begin", "rk_write", "rk_finish", "rk_query", "rk_cancel",
        "mcu_begin", "mcu_write", "mcu_finish", "mcu_query", "mcu_cancel",
        "begin", "write", "finish", "query", "cancel",
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        char json[80];
        (void)snprintf(json, sizeof(json), "{\"action\":\"%s\"}", names[i]);
        cJSON *root = NULL, *body = NULL;
        CHECK(pc_json_parse_object((const uint8_t *)json, strlen(json), &root, NULL) == PC_JSON_OK);
        pc_dispatch_response_t response;
        pc_protocol_frame_t request = {.command_id = PC_COMMAND_FIRMWARE_UPGRADE_REQUEST};
        CHECK(pc_ota_handle(&request, 77U, root, &response) == PC_DISPATCH_RESPONSE_READY);
        cJSON_Delete(root);
        CHECK(response.length > PC_PROTOCOL_HEADER_SIZE && response.data[10] == 0U &&
              response.data[11] == PC_COMMAND_COMMAND_RESP && response.data[15] == 77U);
        CHECK(pc_json_parse_object(response.data + PC_PROTOCOL_HEADER_SIZE,
              response.length - PC_PROTOCOL_HEADER_SIZE, &body, NULL) == PC_JSON_OK);
        CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "errorCode")->valuestring, "notSupported") == 0);
        CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "status")->valuestring, "failed") == 0);
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(body, "action");
        CHECK(i < 5U ? cJSON_IsString(action) && strcmp(action->valuestring, names[i]) == 0 : !action);
        cJSON_Delete(body);
    }
    exit_test(0U);
}
