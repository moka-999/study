# V2 MATS 失败结果联调（2026-10-09）

> 以下保存已安装0.1.30的历史行为（2=FAIL、errorType=null）。用户随后修正为0=FAIL、1=PASS及字符串errorType；新源码和未烧录候选见[结果字段修正](pc_result_fields_20261009.md)，不要用本页旧示例代表新合同。

## 本轮范围

用户确认每次 MATS 都模拟 FAIL，且 RK 空闲、PC 已断开，允许更新 V2。

- 仍由真实 RK 执行算法。按现有协议收齐四 TLV 详情与独立 FAIL 位后，才生成 `0x0105`。
- 只修改 MATS 的 PC 上报视图：`testStatus=2`；若 RK 首错地址为 0，则填演示地址 `0x0000000012345678`，非零地址原样保留。
- 不修改 RK 原始结果或队列数据；其他 19 个算法、受理响应、超时和串口发送失败保持原逻辑。
- `algorithmIndex` 使用 PC 请求值，`testTimeMs` 使用 RK 耗时，`mirrorIndex` 使用 RK 的 `0x3001`。`physicalAddr` 仍为空、`errorType` 仍为 null。
- 沿用 V2 电源/测量模拟和真实 RK；不增加 JSON 字段、不修改正式协议，也不同步到 B1。

**这是故障界面联调固件，不可作为芯片测试结论。重启不会取消强制 FAIL。**

## 修改位置

| 文件 | 修改 |
| --- | --- |
| `App/protocol/pc/pc_test_service.c` | 仅在完整 MATS 结果交付前替换 PC 视图；默认关闭，编译时禁止 B1 或未开启电源模拟的配置 |
| `CMakeLists.txt` | 新增默认 OFF 的 `ENABLE_V2_MATS_FAIL_TEST`，显式启用时给出构建警告 |
| `tools/dev.ps1`、`tools/ota_dev.py` | 增加显式 `-MatsFailTest` / `--mats-fail-test` 参数；普通构建清除此开关 |
| `tests/verify_ota_relocation.py` | 参数传到两个槽的相同构建；产物目录增加 `mats-fail` 防止混用 |
| `tests/verify_pc_rk_service.py`、`tests/test_pc_rk_service.c` | 实际 C/QEMU 覆盖 20 算法的 80 组结果及生命周期保护 |
| `tests/test_ota_dev.py` | 验证新参数传播与三种产物目录隔离 |

编译入口（仅构建，不访问设备）：

```powershell
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ota-build -Version 0.1.30 -Configuration Release -PowerSimulation -MatsFailTest
```

PC 联调完成后，应构建更高版本、**不传 `-MatsFailTest`** 的镜像并重新升级；若仍需原电源/测量模拟，则保留 `-PowerSimulation`。无需修改 PC 的请求格式。

## 0x0105 JSON 示例

以下是格式示例，不是本轮实板算法抓包。index、耗时、站点、DDR 标志与 18 项测量数值仅为示例；测量仍来自现有 V2 模拟模型，无有效采样时按原规则返回 null。

```json
{
  "algorithmType": "functional",
  "algorithmName": "mats",
  "algorithmIndex": 17,
  "testTimeMs": 5645,
  "testResults": [
    {
      "siteId": "",
      "mirrorIndex": 0,
      "testStatus": 2,
      "resultDesc": {
        "firstErrorAddress": "0x0000000012345678",
        "errorType": null
      },
      "physicalAddr": "",
      "measurementData": {
        "Vdd1Ave": 1780, "Vdd1Peak": 1790, "Vdd1Min": 1770,
        "Vdd2Ave": 1080, "Vdd2Peak": 1090, "Vdd2Min": 1070,
        "VddqAve": 580, "VddqPeak": 590, "VddqMin": 570,
        "Idd1Ave": 300, "Idd1Peak": 330, "Idd1Min": 270,
        "Idd2Ave": 140, "Idd2Peak": 155, "Idd2Min": 125,
        "IddqAve": 85, "IddqPeak": 95, "IddqMin": 75
      }
    }
  ]
}
```

`algorithmIndex=17` 仅表示本示例 PC 传入 17，并非要求它等于 RK 的 MATS 算法编号。

## 验证结果

- 先增加断言，确认修改前 MATS 仍上报真实 PASS、测试失败；实现后同一实际 C/QEMU 检查通过。
- 20 算法 × 4 组结果：MATS 连续执行均 FAIL，其他算法不受影响，真实非零地址保留、原结果数据不变，耗时/标志/18 项统计保持原来源。
- 未收齐结果不产生 `0105`；在途 busy、超时、发送失败、断线不补发旧结果、OTA 未确认门禁均检查通过。
- 普通 V2、V2 电源模拟及本源码的 B1 条件门禁回归通过。此项不是独立 B1 工程同步或实板验证。
- PC 协议合同 24 项、分层 19 项、纯接口头检查及 OTA 脚本离线编排通过。
- Release 两槽构建通过；A1→A2 地址修正后与独立 A2 链接逐字节一致，反向 A1 校验通过。

## V2 安装与回读

2026-10-09 17:31 开始，通过既有 TCP OTA 安装；17:33 再次只读确认。

| 项目 | 结果 |
| --- | --- |
| 旧版本/包 ID | `0.1.29 / AE91A07F`，已健康确认 |
| 新版本/包 ID | `0.1.30 / 5520E4C7`，已健康确认 |
| App/OTA 大小 | 148572 / 154620 字节 |
| 地址修正数量 | 1483 项 |
| OTA SHA256 | `6B4D6E4F9E0A94B07CC5B61C7DE95FBD2AAF7D95E772981E892AF4E49C9A2F43` |
| 构建配置 | 完整 OTA ON；V2 电源模拟 ON；MATS FAIL ON；旧 RK 隔离联调 OFF |
| 升级流程 | 3000 字节分块，提交/重复 finish 幂等、重启、0903、30 秒健康确认、心跳通过 |
| W25 测试仪信息 | 重复查询及重连均为 `JMCT-4000 / D00 / RK3588_test` |
| 电源状态 | 升级前后均为内存模拟 OFF；本轮未发送电源控制命令 |

产物与证据：

- 升级包：`build/ota-app-power-sim-mats-fail-0.1.30-release-a1/test.ota`。
- 双槽校验：同目录 `relocation_report.json`；旧 0.1.29 产物保留。
- 升级前：`build/mats_fail_20261009/board_before.json`。
- 升级过程：`build/mats_fail_20261009/ota_upgrade_0.1.30.json`。
- 升级后：`build/mats_fail_20261009/board_after.json`。

所有本轮 TCP 连接已关闭。没有连接 ST-Link/串口、主动下发算法、修改 B1/Bootloader、提交/推送或更新飞书。网络升级依靠原 OTA 双槽和日志保护，本轮未另做整片 Flash 备份。真实 RK 完成后在 PC 显示 MATS FAIL 的现场验收，留给 PC 同事发起。
