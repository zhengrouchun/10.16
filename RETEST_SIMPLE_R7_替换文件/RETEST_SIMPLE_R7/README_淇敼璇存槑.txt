RETEST_SIMPLE_R7 修改说明
=========================

这版不是继续堆状态，而是回到你提供的“07-外部中断”跑通例程的核心结构：
    EXTI4 -> 一个 event flag -> 主循环直接执行检测。

【关键修改】
1. div_control.c
   - 删除 detection_pending 调度。
   - 删除 SEEN_RELEASE / LOW_STREAK / HIGH_STREAK 等复杂路径。
   - PA4 EXTI4 回调只设置 retest_event_pending=1。
   - 主循环收到 RETEST 后：清 STOP 锁存 -> RETEST_ACCEPTED -> DETECTION_START -> DivMeasurement_Run()。
   - STOP 仍最高优先级且立即断电。

2. main.c
   - 固件标识改为 FW=RETEST_SIMPLE_R7。
   - OLED 初始化结束后直接 DivControl_RunInitialDetection()，上电一定尝试首轮1V检测。
   - 默认关闭周期 HB，减少串口/Proteus 仿真负载。

3. div_config.h
   - STATUS_TRACE_ENABLE=0：默认不每秒刷 HB，只输出关键事件和测量结果。
   - UART 仍保持 115200，不要把 Virtual Terminal 改回9600。

4. ADC_CubeMX_Keil.ioc
   - 修复原工程中的重要不一致：原 .ioc 仍写 PA4.Signal=GPIO_Input！
   - 现在同步为 PA4=GPXTI4 + GPIO_MODE_IT_FALLING + GPIO_PULLUP，并启用 EXTI4_IRQn。
   - 这样以后 CubeMX Generate Code 不会把 PA4 又生成回普通输入。

【Proteus接线】
PA4(pin14) --- SW8 --- GND
PA5(pin15) --- STOP --- GND
PA4/PA5 都用内部上拉，不需要额外10k上拉；不要接 LogicState。

【必须看到的启动日志】
FW=RETEST_SIMPLE_R7
随后应出现：
EV=DETECTION_START
STATE=...

【按RETEST后的正确日志】
EV=RETEST_ACCEPTED
EV=DETECTION_START
STATE=...

【按STOP后的正确日志】
EVENT=STOP ...
OLED -> DIV STOPPED
松开STOP后按RETEST，应重新检测。

【串口】
MCU=115200；Proteus Virtual Terminal也必须=115200。
默认关闭HB，所以“没有每秒刷屏”是故意的，不等于主循环卡死。
