# 解决 main.c 职责混杂并完成应用模块拆分

## 原因与目标

原来的 `代码/Src/main.c` 同时实现按键消抖、停止断电、电源互锁、ADC采样、九工况判别、串口报告和OLED刷新，主文件接近九百行。PA4/PA5或OLED故障发生时，输入采样、状态调度和外设输出交错在同一文件里，不容易沿着一个功能定位问题；不同功能还共享了多个静态变量。

这次处理的是代码组织和模块接口问题，不是把某个按键或OLED硬件故障归因为代码问题。保留了R4“先观察到按键释放、再接受稳定按下”的复测逻辑，以及停止优先、先断后合、按需测量等原有流程。OLED、ADC和HAL驱动没有重写。

## 整理结果

- `代码/Src/main.c`：只保留HAL和CubeMX外设初始化、ADC校准、应用模块初始化、主循环调度、生成的时钟函数及错误处理。每个初始化或服务调用旁有用途注释。
- `代码/Src/div_power.c` 与 `代码/Inc/div_power.h`：管理PB12/PB13、停止锁存、紧急断电、等待期间的停止检查和电源互锁。外部中断回调仍只执行紧急断电和请求显示刷新。
- `代码/Src/div_control.c` 与 `代码/Inc/div_control.h`：管理PA4/PA5按键采样、R4消抖计数、诊断快照、停止事件、复测事件和测量调度。SysTick继续调用 `DivButtonsTick`。
- `代码/Src/div_measurement.c` 与 `代码/Inc/div_measurement.h`：管理PA0/PA1 ADC通道采样、数据有效性、工况判别前的数据准备和串口测量报告；检测结果通过显示接口传递，不再直接改显示缓存。
- `代码/Src/div_display.c` 与 `代码/Inc/div_display.h`：管理测量结果缓存、已有OLED驱动初始化、画面刷新、OLED写入进度和运行心跳。
- `代码/Src/div_diagnosis.c` 与 `代码/Inc/div_diagnosis.h`：头文件只保留边界类型、参数和函数声明；mV换算、九工况区间表和唯一匹配算法放在 `.c` 文件。
- `代码/Inc/div_app_types.h`：集中定义跨测量与显示模块使用的测量结果结构。
- `代码/Inc/div_config.h`：集中放置应用模式、超时、引脚、采样和报告周期等参数，并为每个参数保留用途注释。
- `代码/Src/stm32f1xx_it.c`：通过模块头文件调用紧急断电与按键采样；`代码/Inc/main.h` 不再重复声明应用模块接口，CubeMX引脚定义保留。
- `代码/MDK-ARM/ADC_CubeMX_Keil.uvprojx`：将五个新增应用 `.c` 文件加入 Keil 的显式编译清单。

## 使用的方法和指令

先用 `rg --files` 定位当前源码、Keil工程和修改前备份，再读取 `main.c`、中断文件、公共头文件和 Keil 工程的源码分组。模块迁移按函数职责划分，原测量与报告流程从项目备份中恢复后调整为模块接口；R4按键逻辑保留在控制模块。用 PowerShell读取源码进行定点检查，并用 Python `xml.etree.ElementTree`解析 `.uvprojx`，确认工程 XML 可读、所有登记的 C 文件都存在、新模块各登记一次。

本次实际用到的检索/检查命令包括：`rg --files`、`rg -n`、PowerShell `Get-Content`，以及 Python 对 Keil XML 的解析。未修改或重新生成驱动文件，也没有更改 CubeMX 的 GPIO、ADC、USART、I2C或NVIC配置。

## 需要的 CubeMX 设置

这次不需要新增 CubeMX 设置。继续使用工程当前配置：PA4为复测输入、PA5为停止输入并启用下降沿EXTI，PA0/PA1为ADC输入，PB12/PB13为电源控制输出，PB6/PB7按当前工程用于OLED通信。用户无需提供驱动文件。

## 核对情况与限制

已静态核对模块接口定义、Keil工程 XML 和源码清单：新增的 `div_power.c`、`div_control.c`、`div_measurement.c`、`div_display.c`、`div_diagnosis.c` 都已登记，工程引用的 C 文件均存在。当前执行环境没有发现 Keil `UV4`、ARMCC/ARMCLANG 或 Clang 编译器，因此没有完成实际固件编译；本次也没有运行 Proteus 仿真。请在 Keil 中打开 `代码/MDK-ARM/ADC_CubeMX_Keil.uvprojx` 编译，再加载生成的 HEX 到 Proteus，确认启动日志、SW8复测和SW9停止行为。
