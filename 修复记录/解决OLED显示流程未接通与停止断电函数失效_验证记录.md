# 编译与验证记录

日期：2026-10-17。

最终构建日志：`代码/MDK-ARM/解决OLED停留启动画面与断电失效_最终构建.log`。

Keil 结果：0 Error(s), 0 Warning(s)。Code=11684，RO-data=4872，RW-data=84，ZI-data=1868。

HEX 输出大小：46851 字节。HEX 所有行的校验和通过。已在数据中确认 `FW=OLED_STOP_R2`、`OLED_INIT_BEGIN`、`OLED_INIT_END`、`PA5_CFG=`、`OLED_WRITE_PAGE=`。

链接映射确认 `main` 引用 `Oled_Init` 和 `ReportOledProgress`，`ServiceDisplay` 引用 `Oled_Flush`。

`代码/MDK-ARM/ADC_CubeMX_Keil.hex` 与具名副本 `解决OLED显示未刷新与停止断电失效_R2.hex` 的 SHA256 一致：

`9E842A181B944E8615E73F214E9ADFC67E28F8E70BC03916116205AA5062D031`

## 构建期间发现的配置变化

在首次检查和最终构建之间，外部操作更新了 `.ioc` 与 `gpio.c`；本次修复没有修改这两个文件，也没有覆盖这些新变化。

最终构建采用的配置为：PA4 普通输入、内部上拉；PA5 下降沿中断、内部上拉；PB6/PB7 开漏输出；PB12/PB13 开漏输出、无上下拉、初始 High。

其中 PB12/PB13 与原电路的 R5/R6 下拉及高电平使能要求不匹配，需要用户在 CubeMX 改回：GPIO Output Push Pull、初始 Low、No pull-up and no pull-down。PB6/PB7 的软件 I2C 开漏配置保持不变。修改后需要重新生成和编译，以上 HEX 哈希也会变化。

本次具名 HEX 可用于辨认本次应用修复构建，但在 PB12/PB13 配置纠正前不能作为完整电源功能验收固件。

## 尚未完成

没有在 Proteus 中执行新固件。OLED 刷新、按键读数、STOP 后实际断电均需仿真验收。按下引脚实测 0 V 却显示 HIGH 的问题仍未判定根因。

用户最新第二张截图中的按键是断开状态，仅凭这张截图持续 HIGH 不能证明闭合时也读 HIGH。第一张截图显示 BOOT OLED 且仿真时间与第二张不同，不能据此认定它们来自同一次启动。
