> **历史资料，已停用（2026-10-27）**：当前B1/V2已移除MCU Boot和OTA，App从0x08000000直接烧录。下文仅保留当时的设计及验证证据，旧地址、命令和安装步骤不得用于当前镜像。现行操作见项目README/AGENTS中的直接烧录入口。

# V2 / B1 独立网络升级工具

日期：2026-10-15。范围仅电脑端脚本和说明；不改MCU/Boot运行代码、分区或线上字段。
新工具已完成离线及本机回环验证，未用于开发板升级，未提交/推送。

## 给操作者

交付包分V2、B1两份。解压到可写目录，电脑安装Python 3.10+，双击对应`start_*_ota.cmd`。
菜单支持只读查询、网络升级、离线检查包；没有GUI框架或第三方Python依赖。
详细操作分别见[V2说明](../tools/v2_ota_README.md)、[B1说明](../../b1/tools/b1_ota_README.md)。

使用前先独立停止RK算法及B2加热，按原业务流程把RK/DDR关到OFF，断开PC/网络助手。
MCU自身保持供电联网。工具不代替PC控温、不关RK电源、不清busy、不强制复位。
V2虚拟OFF不能证明外接RK空闲。查询只读W25固件/健康和0504逻辑状态；不初始化FRU。

## 给开发者

| 位置 | 职责 |
|---|---|
| `test/tools/ota_client.py` | 公共包检查、18B帧、顺序上传及只读重连确认 |
| `test/tools/v2_ota.py` | V2固定`apollo-v2-h743`入口 |
| `b1/tools/b1_ota.py` | B1固定`b1-h743`入口；源码侧复用同级公共实现 |
| `test/tools/ota_bundle.py` | 各自生成ZIP；将公共实现复制进交付包，运行不依赖源码树 |
| `test/tools/ota_dev.py` | 原V2构建完成后增加工具包输出，旧SWD/网络验收路径不改 |
| `b1/tools/ota_build.py`、`tools/dev.ps1 ota-build` | 复用现有双槽验证及Boot打包器，增加B1开发侧构建入口 |

生成候选交付包（版本号仅示例，后续升级须递增）：

```powershell
# 在D:/pro/test执行；这组参数保留当前V2电源模拟/MATS第一颗假FAIL
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-build -Version 0.1.37 -Configuration Release -PowerSimulation -MatsFailTest

# 在D:/pro/b1执行；真实驱动、UART4，不带V2模拟
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-build -Version 0.1.37 -Configuration Release
```

各工程`build/*-ota-tool-<版本>-<时间>.zip`可独立转交，保留历史包不覆盖。
ZIP内仅对应入口/启动器/说明、公共实现、单个`.ota`和包摘要；不含固件源码、编译器或测试脚本。
固件版本/行为取决于所选包，不由升级工具暗中修改。V2模拟参数需构建者显式指定。

## 安全与失败处理

- 固定板型入口，包头/76B版本信息/修正表/A1/A2 CRC、长度、栈/入口均在发送前检查。
  重用现有包格式与正式打包器，不另起Boot协议；MCU/Boot二次校验不变。
- 单块按begin协商，最大3000字节；Base64、JSON和18B帧头总和不超过4096字节。
  `offset/nextOffset`始终是文件字节偏移。
- 单连接顺序请求，PC/MCU独立Seq；不拿请求Seq认领应答，检查动作、包ID及连续的MCU流水。
  严格拒绝坏头、错误方向、重复JSON键、错误偏移等；接收整帧有时间上限。
- 上传前要求当前固件已健康、版本严格更高、没有接管中的下载且电源OFF；busy等仍由MCU判断。
  旧failed/cancelled记录但运行App健康时允许重新发起，不会自动取消旧receiving/ready事务。
- finish前断线停止，无跨连接续传，也不自动重复write。finish已发送后若应答丢失或commitUnknown，
  只重连发`mcu_query+packageId`；仅0903、对应包、对应版本且confirmed=true才判成功。
- 默认观察约180秒，未确认不等同失败/回滚；保留日志并查版本，不自动补烧或降级。
  日志为逐事件JSONL，含IP/端口、原版本、新包摘要、命令/应答及错误，不存完整Base64固件。
- 查询目前不暴露硬件target，不能仅凭testerName证明目标板型；操作者须核对IP。
  CRC不是签名，只从可信渠道取包。新工具仅支持mcu_*，旧动作迁移仍用已有显式迁移入口。

## 本轮候选与验证

| 板型 | 候选版本 | 包ID | App / 整包字节数 | 地址修正项 |
|---|---|---|---|---|
| V2 | 0.1.37 | B47616AE | 154224 / 160460 | 1530 |
| B1 | 0.1.37 | 45E95E46 | 150380 / 156644 | 1537 |

V2包SHA256：`b0f64fecad8a8a70ba8785bf26d4c681e2e693a88628b2c8501707d16aa3013e`。
B1包SHA256：`760d0af1a44bbba91fc49f9011fdaf35396c5cbf075bd1a3c9c548c7c828dafc`。
均完成Release A1/A2逐字节修正及反向核对；候选未烧录，不代表板上版本已经改变。

`python -B -X utf8 tests/test_ota_client.py`的10组测试通过，包含双板各14种成功/故障路径、
损坏包与互相错板拒绝、不同Seq及回绕、半包、提交后丢应答只读查询、查询观察超时、
ZIP解压到临时目录独立运行，以及两板各一次127.0.0.1真实TCP模拟服务器上传/重连。
原`tests/test_ota_dev.py`离线回归通过；B1 PowerShell入口语法/BOM检查通过。
同一10组回归在Python `-O`模式下通过，外部输入保护不依赖assert；B1原有`dev.ps1 check`全套通过。
两套Windows启动器均已启动菜单并选择退出，未连接设备；交付时固定CRLF换行，兼容UTF-8中文注释。
当前最终交付文件（早先试制目录不作为交付入口）：

- V2：`D:/pro/test/build/v2-ota-tool-0.1.37-20261015-173356-488612.zip`
- B1：`D:/pro/b1/build/b1-ota-tool-0.1.37-20261015-173356-498660.zip`

本轮未连接MCU/串口/ST-Link、未运行RK算法、未操作实际电源、未改Word/飞书或推送。
新工具的真板运行尚待另行确认空闲后验证；B1网口、真实Flash安装及断电/回滚验收仍待板到。
空白板继续使用专用SWD首烧包；App或网络故障无法通过本工具救砖。
