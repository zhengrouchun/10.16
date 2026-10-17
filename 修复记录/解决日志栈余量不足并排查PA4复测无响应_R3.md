# 解决日志栈余量不足并排查 PA4 复测无响应 R3

日期：2026-10-17。

## 当前结论

用户的 R2 仿真截图已证明 PA5 能触发 EVENT=STOP，并把 OLED 刷新为 DIV STOPPED。它证明了停止事件和显示流程响应，不能单凭画面证明外部电源实际切断。

PA4 复测无响应仍未定位。每秒日志显示 HIGH 不足以证明两次打印之间没有短暂低电平。因此 R3 增加累计采样证据，不改变原按键消抖条件或改为别的中断配置。

用户提供的警告是 SDA 线上逻辑冲突和 SDA floating；本组警告没有直接指出 PA4、ADC 或 CPU 指令执行故障。不能把这些问题统归为同一个根因。

## 判别结果核查

直接解析 R2 HEX 并按链接 map 定位 div_bounds，确认九组电压阈值与 div_diagnosis.h 一致。按该实际表核验：VB=0、VA=2 mV 无匹配；VB=455、VA=909 mV 匹配 DIV0；VB=0、VA=833 mV 匹配 DIV4。

只读反汇编核对了 DivButtonsTick、HAL_GPIO_ReadPin 和 RunDivDetection。GPIO 读取函数从 IDR 取值，检测流程包含判别表比较，没有发现源码把近零电压定义为 NORMAL 的证据。

程序仅在上电或接受 PA4 复测后采样，心跳重复的是已有结果。改变 R2 后如果没有成功触发新检测，心跳不会自动改为新的工况。但截图中近零测量值与 DIV0 同时出现仍需继续核查，不能用缓存解释所有矛盾。

## 本次修改的明确问题

原 startup_stm32f103xb.s 分配 1024 字节栈；R2 的 Keil 静态调用图估算最大主调用链为 992 字节，并标注还有未知项。SysTick 中断及硬件异常入栈也要占用栈，原余量不足。没有证据证明截图中的错误结果已经由栈越界造成，因此本次描述为消除风险，不宣称根因已证实。

将 main.c 中 SendReport、SendRetestTrace、ServiceStatus 的日志字符缓冲区改为函数内 static。三个函数均由主循环串行调用，中断不调用这些函数，也不访问这些缓冲区，因此无需为每次调用在栈上分配这些大数组。

R3 链接报告的最大主调用链估算降至 608 字节，仍带未知项；这不是实测最大栈深。ZI-data 增至 3148 字节，换取更大的调用栈余量。

## 新增代码及作用

所有本次新增和修改的应用语句均带中文注释，没有新增驱动或修改 CubeMX 配置，没有引入 0U 形式常量。

- BUTTON_TRACE_BUFFER_SIZE：512 字节诊断缓冲区长度，使用具名常量。
- button_scan_count：记录按钮服务实际执行次数。
- retest_low_count：记录 PA4 被采样为低电平的次数。
- retest_longest_low_ms：记录最长连续低电平持续时间，单位是 HAL 仿真毫秒。
- retest_press_count：记录通过原有消抖和 STOP 条件后产生的有效复测事件次数。
- ++button_scan_count：按键初始化就绪后，每次执行采样递增；不在中断中打印。
- low_duration_ms = now_ms - retest_button.changed_ms：用最近一次电平变化时刻计算本段低电平持续时间。
- ++retest_low_count：仅在真实 HAL 读取结果为低时递增。
- low_duration_ms > retest_longest_low_ms 及后续赋值：保存最长低电平记录，松开后不清零，便于看出短按。
- ++retest_press_count：在原逻辑产生有效按下事件时记录；不额外触发复测。
- 三个 static char line 数组：由栈存储改为静态存储。
- snprintf 新增格式和参数：输出 SCANS、PA4_LOW_SAMPLES、PA4_LOW_MAX_MS、PA4_PRESSES。
- 心跳新增 RESULT=LAST_TEST：说明显示的是最近一次检测结果，并非实时重采样。
- 固件标识改为 FW=BUTTON_TRACE_R3：区分 R2 和本次 R3。

计数从 MCU 复位开始累计；极长运行后无符号累计计数可能回绕。诊断字段读取期间中断仍可更新计数，字段不是同一瞬间的原子快照，不用于精确时序测量。

## 使用到的文件与指令

读取 main.c 的按键、供电、测量、报告和显示流程；stm32f1xx_it.c 的 SysTick 和 EXTI；gpio.c 和 .ioc 的 GPIO 模式；div_diagnosis.h 的阈值；OLED.c 的字节发送与初始化；启动汇编的栈分配；Keil map/htm 的符号和调用栈报告。

仅修改 main.c。修改前副本保存在 `排查PA4复测无响应与降低日志栈占用_R3_修改前备份/main.c`。

使用 rg -n 定位代码；Get-Content 查看上下文；Get-Item/Get-FileHash 核对文件及固件；fromelf --text -c 只读检查实际指令；Python 解析 HEX 校验和、常量表、固件标识和诊断文本上限；apply_patch 修改；UV4.exe -b <工程路径> -o <日志路径> 构建。

## 验证结果

Keil：0 Error(s), 0 Warning(s)。Code=11748，RO-data=4956，RW-data=100，ZI-data=3148。

最大已知主调用链栈估算：608 字节，加未解析项。

诊断格式按全部计数达到 uint32_t 最大值等条件核对：263 字节，小于 512 字节缓冲区。

HEX 行校验和通过；已确认本次版本、累计按键字段和 RESULT=LAST_TEST 字符串存在。

具名固件：`代码/MDK-ARM/降低日志栈占用与排查PA4复测无响应_R3.hex`。

SHA256：`D04751BFCCECE8B5EC1A22D30AD187B67C1C029E3B27A5B58BCA97A2320ACE5C`。

构建日志：`代码/MDK-ARM/降低日志栈占用与排查PA4复测无响应_R3_构建.log`。

没有执行 R3 Proteus 仿真，因此不宣称 PA4 或 NORMAL 异常已解决。

## 用户需完成的电路与配置核对

最终构建时 PB12/PB13 仍是开漏、无上拉、初始 High。按当前电路需要在 CubeMX 改回 Output Push Pull、初始 Low、No pull。再生成和编译后才做供电与故障判别验收；不可只修改 CubeMX 界面而继续加载原 HEX。

PB6/PB7 保持开漏。若模块没有自带上拉，应给 SCL、SDA 分别接上拉电阻到 3.3 V，可先用 4.7 kΩ 作仿真检查，最终需看总线时序。上拉不能保证解决全部逻辑冲突，仍需核对所有总线驱动者。

现有 OLED_I2C_SendByte 在应答时钟前没有释放 SDA，不符合标准发送方在应答时钟期间释放 SDA 的要求。此问题未修改，遵守用户不自行编写驱动的要求；需用户提供合适驱动，或明确允许局部修改现有驱动。

参考：NXP UM10204 第 3.1.1、3.1.6 节，https://www.nxp.com/docs/en/user-guide/UM10204.pdf 。本规范要求不能用于直接证明本次 Proteus 冲突的唯一原因。

## R3 按键检查

停止仿真后加载 R3 并重新运行，先确认 FW=BUTTON_TRACE_R3。保持 STOP 松开，先松开 RETEST，再按住 RETEST，等仿真时间明显推进后松开。

SCANS 应持续增加。PA4_LOW_SAMPLES 始终不增加：本次采样未读到 PA4 低，继续检查引脚输入与仿真。低样本增加而 PA4_LOW_MAX_MS 小于 20：没有观察到足够长的连续低电平。PA4_PRESSES 增加：原按键检测已产生有效事件，继续看 RETEST_ACCEPTED、DETECTION_START 和新 STATE 行。若 STOP 持续有效，原逻辑会拒绝复测，属于预期行为。
