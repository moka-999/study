# B1电压精度排查标注

## 最终采用与弃用范围

内置imagegen的三次PCB截图标注均出现圈点偏移，全部弃用，不用于实板测量，也未作为项目成品保存。前两次把19脚圈到18脚；第三次保留了G但将22脚圈到21脚。下文保留原提示词用于追溯，不代表生成结果已验收。

最终采用独立、按脚号绘制的矢量简图`b1_avdd_pinmap_20261105.svg`及PNG版本；它不是编辑后的PCB照片。脚号由TSSOP-24固定序列逐项排列，已目视核对22是右列第三、19是右列第六。引脚功能依据[ADI AD7124-4 Rev.E表7，PDF第16页](https://www.analog.com/media/en/technical-documentation/data-sheets/ad7124-4.pdf#page=16)。原始DDR实板测点图继续使用`doc/schematic_png/b1_tps_probe_20261103.png`。

## 内置编辑工具的原提示词（结果弃用）

使用内置imagegen编辑工具，输入`D:/pro/test/build/b1_ad_voltage_probe_20261102.png`。原图为MCU面PCB截图，不是实板新照片；不镜像。TSSOP-24的U4028右列从上到下为24至13脚，AVDD=22脚（第三脚），AVSS=19脚（第六脚）。原图白色G位于19脚，原红色4是18脚REFOUT，本次AVDD方案不测REFOUT。

最终提示词：

Edit this existing annotated PCB screenshot into a precise electrical measurement guide. This is an engineering annotation task, not a PCB redesign. Preserve the PCB geometry, component bodies, pad positions, pad count, chip notch, orientation, and neighboring components exactly. Do not mirror or rotate the board. Preserve the MCU-side view. Use the original screenshot as the immutable background; only change annotation overlays.

The source image is 1672 by 941. U4028 is the large vertical 24-pin IC near the right side. Its right-hand row has exactly 12 horizontal cyan pads. From top to bottom their pin numbers are 24,23,22,21,20,19,18,17,16,15,14,13. The third pad from the top of this right-hand row is pin 22, AVDD, centered approximately at original pixel (1421,291). The sixth pad is pin 19, AVSS, centered approximately at (1421,382), currently marked by the original white G circle. These two existing pads are the ONLY measurement targets for this guide. Do not place a marker on the fourth pad or on the seventh pad. The original red 4 at the seventh pad marks REFOUT pin 18 and must not be recommended for this measurement.

Add a clear red open circle centered on the existing third right-hand pad (22 AVDD), with a short precise red leader to a white-backed label reading exactly "红笔：22脚 AVDD". Add a clear dark/white open circle at the existing sixth right-hand pad (19 AVSS), replacing the original G overlay if necessary, with a short precise dark leader and white-backed label reading exactly "黑笔：19脚 AVSS". Keep leader endpoints unambiguous and labels away from the contact points. Retain all cyan pad shapes without creating, deleting or shifting any pad. The original red 1/2/3/4 circles may be removed as annotation overlays only, or muted; do not alter the underlying pad geometry.

Replace the obsolete lower-left white instruction box with a clean white box containing exactly this concise Chinese text, with readable typography:
"B1 · 本轮测AD基准供电"
"MCU面，不要再次镜像"
"红笔：22脚AVDD，右列从上第3脚"
"黑笔：19脚AVSS，右列从上第6脚"
"万用表：直流V；红线VΩ，黑线COM"
"记录实际读数，不预填3.333V"
"本轮不测18脚REFOUT"
"细表针一次碰一脚，避免短接相邻脚"

Use a landscape high-resolution output matching the original aspect ratio, no decorative elements, no invented readings, no redesigned traces or components. Make the red positive and black negative contact points absolutely clear, third and sixth pads of the right row respectively.
