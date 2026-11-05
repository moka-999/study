# PC电压字段改为字符串mV（2026-10-12）

2026-11-04补充：用户批准B1验板0509的OFF语义扩展，命令号和四字段不变。OFF接受完整合法三轨目标并只保存RAM，下次0501上电直接写入TPS；ON仍为同类型实时调压，不覆盖下次上电目标。MCU复位恢复标称，非法目标不改变旧缓存。V2未同步该扩展，010x不变；下文关于当时OFF仅标称的记录不再适用于新B1固件。工具及硬件边界见[验板工具](../tools/b1_debug/README.md)。

## 1. 本次合同

按用户确认，0103的`power`和0509请求共用以下四字段；替代旧的整数`vdd1Steps/vdd2Steps/vddqSteps`，不增加帧头字段或命令号。

```json
{
  "memoryType": "LPDDR4X",
  "vdd1": "1800",
  "vdd2": "1100",
  "vddq": "600"
}
```

电压单位为mV，JSON类型必须为string。值只能包含ASCII数字0～9且非空；不接受正负号、空白、小数、指数、单位、JSON数字或null。允许前导零，但不赋予特殊含义。字符串`"0"`不是保持原值，而是越界。

`目标mV = 所选内存类型的该轨标称mV + 原生步进mV × n`，其中n为有符号整数；MCU先检查精确整档，再检查完整可写范围，不做四舍五入。

| 电源轨 | LPDDR4标称mV | LPDDR4X标称mV | 原生步进mV | 完整可编码范围mV |
|---|---:|---:|---:|---:|
| VDD1 | 1800 | 1800 | 10 | 800～3350 |
| VDD2 | 1100 | 1100 | 5 | 400～1675 |
| VDDQ | 1100 | 600 | 5 | 400～1675 |

例如LPDDR4X的1810/1105/605mV有效；VDD1=1805mV不是整档，返回`0006 failed/invalidArgument`。字段缺失、重复、额外字段、旧Steps字段、格式错误和越界同样拒绝。全部三轨校验通过后才交付目标，失败不改变原配置。

完整可编码范围不是DDR安全范围。本轮未添加或放宽安全白名单，不批准极限扫压；具体颗粒范围、板端精度/纹波和训练条件仍需实板验收。原生步进的器件依据见[前一阶段记录](ddr_native_steps_20261012.md)。

## 2. 业务边界不变

- 0103仍将power解释为训练目标；`power:null/bootMode:null/reboot:false`外部已启动模式保留。
- 0509仍在OFF时仅允许所选类型标称值，ON时仅允许同类型调压；busy、OTA健康、电源状态和真实RK门禁不变。
- 用户的`parametric/idd_0`示例仅确认JSON格式，不代表IDD算法或完整B1训练流程已经实现。
- 内部保留既有有符号档位和TPS驱动接口，由PC边界层将绝对mV精确转换；不改寄存器、引脚、电源时序或RK线格式。
- V2模拟和每次MATS第一颗假FAIL保持原开关，第二颗真实；B1继续禁止模拟/假FAIL。
- PC必须和新固件配套切换。旧Steps包在新固件中拒绝，新mV字符串包不能直接用于旧板上固件。

## 3. 修改范围

- V2与独立B1的`pc_test_message.c/.h`增加共用`pc_power_parse()`，0103及0509分发复用，不保留两套转换。
- 同步实际C用例、B1本地解析测试入口、静态合同、Mock与网络脚本请求样例；网络脚本本轮未连接设备。
- 20个网络助手算法包重算JSON、完整HEX及总长度：前10项1810/1105/605mV，后10项1790/1095/595mV。
- 更新两板PC Markdown、需求/接手说明及本轮任务状态；历史记录不改写为当前事实。

## 4. 验证结果

| 检查 | 结果与边界 |
|---|---|
| V2/B1实际C解析（QEMU） | 各覆盖2种内存×3轨×256码=1536个单轨合法目标，不是穷举全部三轨笛卡尔组合；另测边界、非整档、类型/格式/旧字段及失败输出不变 |
| V2普通PC–RK服务 | 通过既有实际C回归，协议/分发未脱离正式路径 |
| V2电源模拟、模拟加MATS假FAIL | 均通过；80组MATS结果隔离，非法电压不改变模拟目标 |
| B1真实业务门禁 | 通过ON门禁、100次查询、18项统计及部分采样/冻结检查；不等于真板测试 |
| V2/B1普通构建 | 均通过；V2驱动Mock镜像也构建通过，未上板执行 |
| PC及分层检查 | V2 PC合同24项、分层19项及独立接口头检查通过；B1 check及本地解析C入口通过 |
| 网络助手清单 | 20组JSON/HEX、帧头、长度、Seq和两种查询包校验通过 |

主要入口：`tests/verify_pc_start_c.py`（可加`--app-root ../b1`）、`tests/verify_pc_rk_service.py`（普通、`--power-simulation`、再加`--mats-fail-test`、或`--app-root ../b1 --b1-guard`）、`tests/verify_func_packet_examples.py`；两板另运行各自`tools/dev.ps1 build`，B1运行`check`，V2运行`pc-protocol-test/style-test/driver-mock-build`。这些检查使用实际C及受控替身，不验证TPS实物、电压精度或DDR训练。

## 5. Word与飞书

- Word最新文件保持`doc/pc_tcp协议.docx`，用户关闭后才替换。旧稿按原修改时间归档为`doc/history/pc_tcp/pc_tcp协议_20261012_102554.docx`，SHA-256为`7732172cd92dafcbd1026c71781b6d0468a8159c602de3b0d064802cfe576b89`。
- Word仅修改0103/0509的10个段落和3行字段表，24个JSON示例严格解析通过。默认渲染器缺少LibreOffice，改用WPS后台只读导出19页，再逐页查看；无截字、重叠或越界，仅第6/11页像素变化，其余17页与前版一致。检查材料在`tmp/feishu_pc_voltage_20261012/render/`。
- Word此前尚未同步的双颗0105、TCP恢复等内容没有借本轮重写；这次是电压部分同步，不宣称整份Word与最新实现完全一致。后续应独立收齐这些已确认内容，当前完整合同以Markdown和飞书为准。
- [现有飞书PC协议](https://tcn47lisvzcv.feishu.cn/docx/GWdYdgSArojCm3xPAPzcJxbxnqh)先读到版本256，其中同事已经修改0103示例和字段类型；保留其内容，仅补齐说明及0509。9项局部写入均成功且无警告，最终版本265；两节逐项回读符合计划，未重写其他章节。证据在`tmp/feishu_pc_voltage_20261012/`，最终快照为`final_start.json/final_power.json`。未另做飞书浏览器图面检查。

本轮没有连接MCU的TCP、串口或ST-Link，没有烧录、提交、推送。下一步需在用户确认设备空闲后构建并安装配套OTA，再验证真实网络收发；新协议不能提前套用到旧固件。

## 6. V2安装与网络检查（同日后续）

用户随后要求烧录，并确认RK空闲、PC/网络助手已断开。先经正式TCP只读查询：原固件0.1.33、包1287728B、confirmed，虚拟电源OFF。无需关电命令或额外复位；沿用网络OTA，由原B安装，不占用ST-Link、不重烧B、不改实际电源或重启RK。

| 项目 | 结果 |
|---|---|
| 新版本/包ID | 0.1.34 / E6DA3DA5 |
| 配置 | Release；完整OTA、电源/测量模拟、MATS第一颗假FAIL均ON；隔离网关、驱动Mock及波形自检OFF，RK仍走真实串口 |
| 地址修正 | App为150020字节，1477处修正；A1→A2逐字节一致，反向A1一致 |
| 升级文件 | `build/ota-app-power-sim-mats-fail-0.1.34-release-a1/test.ota`，156044字节 |
| SHA-256 | `e9b0d70097a9d71e37e09ba7a2fc4b2ceacbd53e6597fc175bf5a0cd4ca4e272` |
| 安装确认 | 按3000字节原始数据分块上传，完成后重启；目标包0903、30秒健康及心跳通过 |
| 重连查询 | W25返回0.1.34/confirmed及包E6DA3DA5；测试仪仍为JMCT-4000/D00/RK3588_test |
| 电源/资源 | 虚拟电源前后OFF、循环计数0，采样无有效值；脚本退出已关闭TCP，无ST-Link或串口占用 |

升级后通过29次网络请求检查：

- 0103合法的字符串mV：标称、各升一档、各降一档、LPDDR4类型共4组，通过参数解析后返回`notSupported`。这些请求专用当前未实现的`parametric/idd_0`，不会投递RK算法或改变虚拟电源。
- 9种错误电压对象分别用于0103/0509：JSON数字、三轨非整档、上下越界、单位后缀、空白及旧Steps字段，均返回`invalidArgument`。
- 0509在OFF时接收LPDDR4X的1800/1100/600mV并返回`completed`，仅选择标称类型、不写真实TPS；OFF时VDD1=1810mV为合法整档，但状态不允许，返回`invalidState`。
- 错误请求后同一TCP连接仍能查询状态/采样、响应心跳；前后OFF和循环计数均不变。

证据：`build/voltage_mv_before_20261012.json`、`build/voltage_mv_ota_0134_20261012.json`、`build/voltage_mv_after_20261012.json`及`build/voltage_mv_live_contract_20261012.json`。本次网络方式未额外读取整片备份，沿用现有Boot旧槽/日志保护；没有宣称内部Flash全片回读验收。

V2现在可使用新电压JSON，不再接受旧Steps字段。未启动真实RK算法，未验证真实TPS电压/DDR训练；B1未烧录，未提交或推送仓库。
