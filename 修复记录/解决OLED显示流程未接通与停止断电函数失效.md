# 解决 OLED 显示流程未接通与停止断电函数失效

日期：2026-10-17。

## 状态与范围

本次修复当前工作目录中已确认的应用代码缺陷。按键在 Proteus 中按下后仍读 HIGH 的原因尚未确认，不能把本次编译通过等同于仿真验收通过。用户已确认 SW9 可以自由打开。

没有修改 OLED 驱动、GPIO 配置、CubeMX 工程或 Proteus 电路；没有增加驱动文件。

## 已确认的问题和原因

1. `代码/Inc/oled_display.h` 中 `OLED_DISPLAY_ENABLE` 为 0，主循环的显示服务没有参与编译。
2. `代码/Src/main.c` 没有调用已有的 `Oled_Init()`，只打开编译开关也无法建立刷新所需的初始化状态。
3. `power_gpio_ready` 初始化为 0 后没有置为 1，导致 `AllPowerOff()` 跳过真正的 PB12、PB13 断电写操作，破坏停止断电与电源先断后合流程。
4. 原日志固定写 `INPUT_DIAG=EXT_PULLUP_R1`，但当前 `.ioc` 和 `gpio.c` 都配置内部上拉，版本文字不能作为实际配置证据。

新截图的 OLED 已显示 BOOT OLED，证明截图那次运行至少完成过启动文字写入；这不能证明当前目录中仍禁用显示的源码就是该截图的构建来源。另一张图的 PA4_CFG=0x8 与当前内部上拉配置一致。两张截图的仿真时间不同，不应直接拼成同一次运行的执行顺序。

## 修改及每条新增语句的作用

- `#define OLED_DISPLAY_ENABLE 1`：启用受该条件编译控制的测量缓存和屏幕刷新。
- `power_gpio_ready = 1;`：在外设初始化完成后允许断电函数实际操作电源 GPIO，随后原有 `AllPowerOff()` 立即关闭两路输出。
- `Oled_SetProgressCallback(ReportOledProgress);`：启用工程已有的进度回调，每完成一行就报告已写入的显存页数，便于定位刷新停滞。
- `SendRetestTrace("OLED_INIT_BEGIN");` 和 `SendRetestTrace("OLED_INIT_END");`：分别在 OLED 初始化前后输出阶段标记，区分初始化未返回与后续刷新未完成。
- `Oled_Init();`：调用已有封装和已有 OLED 驱动，初始化屏幕及显示缓存；按键消抖在此之前已就绪。
- `char line[320];`：扩大按键日志缓冲区，容纳新增的 PA5 配置与固件版本。
- `uint32_t stop_config = ...;`：从已有 GPIOA 配置快照中提取 PA5 对应配置位，使用 STM32 头文件里的具名位掩码和位置定义。
- 修改诊断格式及其参数：输出 `PA5_CFG`，并保持格式占位符与参数逐项对应。
- 修改启动和周期日志：统一使用 `FW=OLED_STOP_R2`，不再用固定文字声称实际使用外部上拉。

新增、修改的应用语句均带中文作用注释，没有引入 `0U` 形式的常量。STM32 原有厂商驱动没有改写。

## 阅读文件及使用位置

- `代码/Src/main.c`：`AllPowerOff`、`SelectPower`、`DivButtonsTick`、`SendRetestTrace`、`ServiceControls`、`ServiceDisplay` 和 `main` 初始化流程。
- `代码/Inc/oled_display.h`：显示开关、已有接口声明。
- `代码/Src/oled_display.c`：`Oled_Init` 设置初始化标志及 `Oled_Flush` 刷新条件。
- `代码/Src/gpio.c`：PA4/PA5 上下拉与 PA5 下降沿中断配置。
- `代码/ADC_CubeMX_Keil.ioc`：PA4/PA5 的配置源及 EXTI9_5 使能；本次检查仍为内部上拉。
- `代码/Src/stm32f1xx_it.c`：SysTick 中调用 `DivButtonsTick`，EXTI 中调用 HAL 停止中断处理。
- `代码/Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xb.h`：确认 PA5 配置掩码及位位置宏存在。
- `代码/MDK-ARM/ADC_CubeMX_Keil.uvprojx`：实际编译目标和输出路径。

## 使用方法与指令

先用 `rg -n` 定位显示开关、初始化调用、断电就绪标志和按键配置；用 `Get-Content` 核对上下文，用 `Get-Item` 对照源码和 HEX 修改时间。

通过 `Copy-Item` 备份两个待修改文件，使用 `apply_patch` 作小范围修改。

使用本机 Keil `UV4.exe -r <工程绝对路径> -o <日志绝对路径>` 完整重建。通过 PowerShell `Start-Process -WindowStyle Hidden` 启动，避免打开额外操作窗口。

通过 `Get-Content -Tail` 检查构建结果，通过解析 HEX 中的数据记录核对固件版本文字，以 `Get-FileHash -Algorithm SHA256` 比较具名固件与工程输出一致性。

## 仿真验收顺序

1. 停止 Proteus 仿真，选择本次具名 HEX，再重新运行。
2. 串口应出现 `FW=OLED_STOP_R2`。终端与工程均使用 9600、8 数据位、无校验、1 停止位。
3. 若只有 BOOT OLED，没有新的版本日志，记录从启动开始的串口内容及 Proteus Messages；不要直接认定按键故障。
4. 两键松开时记录 PA4、PA5、PA4_CFG、PA5_CFG。按住 SW8 时记录是否变 LOW，以及是否出现 RETEST_ACCEPTED、DETECTION_START。
5. 按下 STOP 时确认 PA5=LOW、EVENT=STOP，并实际检查 PB12、PB13 均为低电平。不能只凭串口文字判定电源已经断开。
6. 松开 STOP 后不会自行恢复；松开 SW8 后重新按下 SW8 才请求复测。OLED 应随测量或停止状态更新。

若芯片端实测 0 V 而同一时刻日志仍是 HIGH，需继续核对 GPIO 输入和仿真模型；本次没有把这种现象归因于未经验证的原因。

外部上拉对照试验必须同时满足：保留 RK1/RK2 到 3.3 V 的电阻，且 PA4/PA5 配为 No pull。最新电路截图没有看到这些电阻，因此不能直接要求取消内部上拉。

## 备份与验证记录

修改前文件保存在同目录 `修复OLED停留启动画面与断电失效_修改前备份`。完整重建日志位于 `代码/MDK-ARM/解决OLED停留启动画面与断电失效_重建.log`。实际编译结果与固件校验值另见同目录验证记录。
