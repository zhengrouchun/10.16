/* 解决应用参数散落在 main.c：集中保存检测、按键和串口参数。 */
#ifndef DIV_CONFIG_H
#define DIV_CONFIG_H
#include "main.h"

#define DIV_REFERENCE_SERIAL_MODE 0 /* 1=PB12使能1V测试并按截图连续报告，不切3V；0=原正常工况切3V流程。 */
#define ADC_TIMEOUT_MILLISECONDS 20 /* 最多等待一次 ADC 转换 20 ms。 */
#define UART_TIMEOUT_MILLISECONDS 1000 /* 一条报告最多等待1000ms；9600波特发送约300字节需要约313ms。 */
#define REPORT_BUFFER_SIZE 384 /* 容纳整条带状态和错误信息的报告。 */
#define BUTTON_TRACE_BUFFER_SIZE 512 /* 容纳按键电平、配置、累计采样和最长低电平时间。 */
#define TEST_POWER_PIN GPIO_PIN_12 /* PB12，芯片第 25 脚，控制 1 V 测试电源开关。 */
#define RUN_POWER_PIN GPIO_PIN_13 /* PB13，芯片第 26 脚，控制 3 V 运行电源开关。 */
#define RETEST_BUTTON_PIN GPIO_PIN_4 /* PA4，芯片第 14 脚，复测按钮按下接地。 */
#define STOP_BUTTON_PIN GPIO_PIN_5 /* PA5，芯片第 15 脚，停止按钮按下接地。 */
#define POWER_BREAK_MILLISECONDS 5 /* 两路电源都断开后等待 5 ms，保证先断后合。 */
#define TEST_POWER_OBSERVATION_MILLISECONDS 1500 /* 让1V检测输出保持1.5秒，供电压表观察并让DIV电压稳定。 */
#define BUTTON_DEBOUNCE_MILLISECONDS 20 /* 复测按钮稳定 20 ms 后接受一次按下事件。 */
#define CONTROL_POLL_MILLISECONDS 1 /* 主循环每隔 1 ms 检查复测；停止由中断立即断电。 */
#define RETEST_TRACE_INTERVAL_MILLISECONDS 1000 /* 诊断期间每秒确认主循环仍在运行，单位为仿真毫秒。 */
#define STATUS_INTERVAL_MILLISECONDS 1000 /* 每秒报告运行状态及I2C引脚，便于确认主循环仍在运行。 */
#define AUTO_MEASURE_INTERVAL_MILLISECONDS 0 /* 零表示仅启动时及按PA4复测时检测，不自动重复采样。 */
#define TERMINAL_STARTUP_WAIT_MILLISECONDS 50 /* 上电后给Proteus虚拟终端50ms建立接收状态。 */
#define DIV_NORMAL_CASE 0 /* 正常工况为 DIV0，只有该状态允许进入运行。 */


#endif /* DIV_CONFIG_H */
