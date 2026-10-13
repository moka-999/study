# B1结果DDR标志同步

按2026-10-09确认的PC 0105定义，mirrorIndex原样采用RK 0x3001的uint8值0..255，0也是有效值；不再作为B2/测试机编号。physicalAddr保留为空字符串，含义暂不定义。siteId、首错地址、测试状态、算法耗时和测量统计不变。

同步`pc_test_service.c`和PC/RK接口中文注释、当前协议文档及AGENTS。真实电源ON门禁、真实采样、RK Seq窗口和结果聚合均未改动。

B1实际C/QEMU服务回归覆盖0、1、9、255，0/1/255分别验证PASS/FAIL，physicalAddr固定为空；JSON组包回归、Debug构建、工程check均通过。未烧录B1，未重打首烧包，未提交或推送。

公共PC Word及完整验证记录见[V2修改记录](../../test/doc/pc_ddr_marker_20261009.md)。
