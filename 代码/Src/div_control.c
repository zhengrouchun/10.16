/* 解决按键消抖和业务调度堆在 main.c：控制模块实现。 */
#include "main.h"
#include "usart.h"
#include "div_config.h"
#include "div_power.h"
#include "div_control.h"
#include "div_measurement.h"
#include "div_display.h"
#include <stdio.h>
#include "div_diagnosis.h"
typedef struct /* 保存复测按钮的消抖状态，持续按下只触发一次。 */
{
    GPIO_PinState raw_level; /* 最近一次读到的按钮电平。 */
    GPIO_PinState stable_level; /* 已连续稳定足够时间的按钮电平。 */
    uint32_t changed_ms; /* 原始电平最后变化的时刻。 */
} RetestButton; /* 为复测按钮的状态定义类型名。 */

static int detection_pending = 1; /* 启动后安排一次初始测量。 */
static int stop_reported = 0; /* 防止同一次停止重复输出。 */
static RetestButton retest_button = {GPIO_PIN_SET, GPIO_PIN_SET, 0}; /* 保存PA4消抖状态。 */
static volatile int buttons_ready = 0; /* SysTick按键服务初始化标志。 */
static volatile int retest_event_pending = 0; /* 锁存待处理的复测事件。 */
static volatile int retest_armed = 1; /* 复测事件布防状态。 */
static volatile uint32_t button_scan_count = 0; /* 累计按键扫描次数。 */
static volatile uint32_t retest_low_count = 0; /* 累计PA4低电平采样次数。 */
static volatile uint32_t retest_longest_low_ms = 0; /* 最长连续按下时长。 */
static volatile uint32_t retest_press_count = 0; /* 累计有效复测次数。 */
static volatile uint16_t retest_low_streak = 0; /* 连续低电平样本数。 */
static volatile uint16_t retest_high_streak = 0; /* 连续高电平样本数。 */
static volatile uint8_t retest_seen_released = 0; /* 启动后是否确认过释放。 */
static volatile uint8_t retest_latched_press = 0; /* 防止长按重复触发。 */
#if AUTO_MEASURE_INTERVAL_MILLISECONDS > 0 /* 自动复测启用时才需要记录时间。 */
static uint32_t last_auto_measure_ms = 0; /* 最近一次自动检测完成时刻。 */
#endif /* 结束按需复测模式的条件编译。 */
void DivControl_InitializeButtons(void) /* ADC校准后初始化按钮并启动SysTick采样。 */
{
    retest_button.raw_level = HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN); /* 上电按住不产生虚假的按下沿。 */
    retest_button.stable_level = retest_button.raw_level; /* 记录初始稳定电平。 */
    retest_button.changed_ms = HAL_GetTick(); /* 记录消抖起始时间。 */
    retest_seen_released = 0; /* 先连续观察到释放才重新布防。 */
    retest_low_streak = 0; /* 清空低电平计数。 */
    retest_high_streak = 0; /* 清空高电平计数。 */
    buttons_ready = 1; /* 允许SysTick执行按键扫描。 */
}
void DivButtonsTick(void) /* SysTick每毫秒采样按键，只更新状态，不发送数据。 */
{
    GPIO_PinState current_level; /* 保存本次PA4原始电平。 */
    GPIO_PinState stop_level; /* 保存本次PA5停止电平。 */
    uint32_t now_ms; /* 保存本次采样时刻。 */

    if (!buttons_ready) /* 按键初态尚未初始化时不读取输入。 */
    {
        return; /* 等待主程序开放按键扫描。 */
    }

    current_level = HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN); /* 读取低有效的PA4复测输入。 */
    stop_level = HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN); /* 读取低有效的PA5停止输入。 */
    now_ms = HAL_GetTick(); /* 取得系统毫秒计数。 */
    ++button_scan_count; /* 记录SysTick按键服务确实执行。 */

    if (current_level != retest_button.raw_level) /* 原始电平变化时重置消抖计时。 */
    {
        retest_button.raw_level = current_level; /* 保存新的PA4原始电平。 */
        retest_button.changed_ms = now_ms; /* 将稳定时间起点设为当前时刻。 */
    }

    if (current_level == GPIO_PIN_RESET) /* 按下状态按低电平计数。 */
    {
        uint32_t duration = now_ms - retest_button.changed_ms; /* 计算这次连续低电平持续时间。 */
        ++retest_low_count; /* 累计PA4低电平采样次数。 */
        if (duration > retest_longest_low_ms) /* 检查是否刷新最长按住时长。 */
        {
            retest_longest_low_ms = duration; /* 保存本次更长的连续按住时长。 */
        }
        if (retest_low_streak < BUTTON_DEBOUNCE_MILLISECONDS) /* 消抖计数最多增加到阈值。 */
        {
            ++retest_low_streak; /* 累计一个连续低电平样本。 */
        }
        retest_high_streak = 0; /* 低电平打断连续释放计数。 */
    }
    else /* 松开状态按高电平计数。 */
    {
        if (retest_high_streak < BUTTON_DEBOUNCE_MILLISECONDS) /* 释放计数最多增加到阈值。 */
        {
            ++retest_high_streak; /* 累计一个连续高电平样本。 */
        }
        retest_low_streak = 0; /* 高电平打断连续按下计数。 */
    }

    if (stop_level == GPIO_PIN_RESET) /* STOP按下时不接受任何复测事件。 */
    {
        retest_event_pending = 0; /* 清除已经排队的复测请求。 */
        retest_armed = 0; /* 停止期间解除复测布防。 */
        retest_seen_released = 0; /* 停止后要求重新确认按键释放。 */
        retest_latched_press = 0; /* 清除旧按压锁存状态。 */
        return; /* 停止优先，结束本次按键采样。 */
    }

    if (retest_high_streak >= BUTTON_DEBOUNCE_MILLISECONDS) /* 连续释放达到20ms后重新布防。 */
    {
        retest_button.stable_level = GPIO_PIN_SET; /* 更新消抖后的稳定电平。 */
        retest_seen_released = 1; /* 记录这次启动以来已观察到释放。 */
        retest_armed = 1; /* 允许之后产生新的按下事件。 */
        retest_latched_press = 0; /* 解除上一次长按锁存。 */
    }

    if (retest_low_streak >= BUTTON_DEBOUNCE_MILLISECONDS) /* 连续按下达到20ms后确认按键事件。 */
    {
        retest_button.stable_level = GPIO_PIN_RESET; /* 更新消抖后的稳定电平。 */
        if (retest_seen_released && retest_armed && !retest_latched_press) /* 只接受释放后的第一次有效按下。 */
        {
            retest_event_pending = 1; /* 将按下事件留给主循环执行。 */
            ++retest_press_count; /* 累计确认过的复测按下次数。 */
            retest_armed = 0; /* 本次按下消费掉复测布防状态。 */
            retest_latched_press = 1; /* 防止持续按住时重复触发。 */
            retest_seen_released = 0; /* 下次触发前必须再次确认释放。 */
        }
    }
}

static int RetestWasPressed(void) /* 主循环原子取走消抖后的按下事件。 */
{
    uint32_t saved_mask = __get_PRIMASK(); /* 保存进入临界区前的中断状态。 */
    int pressed; /* 保存要交付的事件状态。 */
    __disable_irq(); /* 防止读取期间被SysTick修改。 */
    pressed = retest_event_pending; /* 读取已经锁存的复测事件。 */
    retest_event_pending = 0; /* 清除事件，防止主循环重复执行。 */
    __set_PRIMASK(saved_mask); /* 恢复原中断状态。 */
    return pressed; /* 返回是否收到复测事件。 */
}
void DivControl_Trace(const char *event) /* 通过独立事件行指出复测流程实际到达的位置。 */
{
    static char line[BUTTON_TRACE_BUFFER_SIZE]; /* 使用静态缓冲，容纳按键诊断字段并避免增加调用栈。 */
    uint32_t input_snapshot = GPIOA->IDR; /* 直接保存 GPIOA 输入寄存器，用于和外部电压及 HAL 读数对照。 */
    uint32_t config_snapshot = GPIOA->CRL; /* 保存 PA0 至 PA7 的实际配置，检查 PA4 是否被其他初始化覆盖。 */
    uint32_t retest_config = (config_snapshot & (GPIO_CRL_MODE4 | GPIO_CRL_CNF4)) >> GPIO_CRL_MODE4_Pos; /* 提取 PA4 的四位配置；使用芯片头文件提供的位名，避免硬写移位值。 */
    uint32_t stop_config = (config_snapshot & (GPIO_CRL_MODE5 | GPIO_CRL_CNF5)) >> GPIO_CRL_MODE5_Pos; /* 从同一次配置快照中提取 PA5 模式，用于核对 CubeMX 配置是否生效。 */
    int written = snprintf(line, sizeof(line), /* 将一次输入快照和独立 HAL 读取组成完整诊断记录。 */
        "EVENT=%s TICK_MS=%lu PA4=%s PA5=%s ARMED=%s PENDING=%s STOP_LATCH=%s GPIOA_IDR=0x%04lX PA4_CFG=0x%lX PA5_CFG=0x%lX HAL_PA4=%s SCANS=%lu PA4_LOW_SAMPLES=%lu PA4_LOW_MAX_MS=%lu PA4_PRESSES=%lu FW=BUTTON_FIX_R4 RAW=%s STABLE=%s SEEN_RELEASE=%u LOW_STREAK=%u HIGH_STREAK=%u\r\n", /* 汇总输入快照和R4按键消抖证据。 */
        event, (unsigned long)HAL_GetTick(), /* 填入事件名和当前毫秒计时。 */
        (input_snapshot & RETEST_BUTTON_PIN) != 0 ? "HIGH" : "LOW", /* 用输入快照的 PA4 位报告复测电平，零表示低电平。 */
        (input_snapshot & STOP_BUTTON_PIN) != 0 ? "HIGH" : "LOW", /* 用同一次快照的 PA5 位报告停止电平。 */
        retest_armed ? "YES" : "NO", retest_event_pending ? "YES" : "NO", DivPower_StopIsActive() ? "YES" : "NO", /* 填入允许复测、排队事件和停止锁存状态。 */
        (unsigned long)input_snapshot, (unsigned long)retest_config, (unsigned long)stop_config, /* 输出原始输入值及 PA4、PA5 的实际配置。 */
        HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN) == GPIO_PIN_SET ? "HIGH" : "LOW", /* 独立读取PA4并与寄存器快照对照。 */
        (unsigned long)button_scan_count, (unsigned long)retest_low_count, /* 输出采样总数和低电平样本数。 */
        (unsigned long)retest_longest_low_ms, (unsigned long)retest_press_count, /* 输出最长按住时长和有效按下次数。 */
        retest_button.raw_level == GPIO_PIN_SET ? "HIGH" : "LOW", /* 输出PA4原始采样电平。 */
        retest_button.stable_level == GPIO_PIN_SET ? "HIGH" : "LOW", /* 输出消抖后的稳定电平。 */
        (unsigned int)retest_seen_released, (unsigned int)retest_low_streak, (unsigned int)retest_high_streak); /* 输出释放资格和连续采样计数。 */
    if (written <= 0 || written >= (int)sizeof(line)) /* 拒绝发送格式化失败或被截断的诊断记录。 */
    {
        Error_Handler(); /* 保持原有错误时两路断电的处理方式。 */
    }
    if (HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)written, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 只在主循环发送，避免在 SysTick 中阻塞。 */
    {
        Error_Handler(); /* UART 发送失败时保留故障现场。 */
    }
}

static void SendStopEvent(void) /* 报告停止事件，与用于填写 CSV 的 STATE 检测记录分开。 */
{
#if DIV_REFERENCE_SERIAL_MODE /* 停止时拉低PB12/PB13并暂停采样，但不能冒称已断开图中的手动SW1。 */
    const char stopped[] = "EVENT=STOP RUN_POWER=NOT_IMPLEMENTED MODE=WAIT_RETEST\r\n"; /* 不将外接电源误报为已断电。 */
#else /* 原受控电源模式保留断电事件格式。 */
    const char stopped[] = "EVENT=STOP RUN_POWER=OFF TEST_POWER=OFF MODE=WAIT_RETEST\r\n"; /* 表明停止后必须重新按复测。 */
#endif /* 结束停止事件格式选择。 */
    if (HAL_UART_Transmit(&huart1, (uint8_t *)stopped, sizeof(stopped) - 1, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 在主循环里发送，中断只负责断电。 */
    {
        Error_Handler(); /* 通信失败也维持双路断电并停机。 */
    }
}

void DivControl_Service(void) /* 停止优先；按需模式只在启动和有效复测后安排检测。 */
{
    static GPIO_PinState previous_retest_level = GPIO_PIN_SET; /* 保存主循环上次看到的 PA4 电平，用于只报告按下沿。 */
    static uint32_t previous_trace_ms = 0; /* 保存上次主循环存活报告的时刻。 */
    GPIO_PinState current_retest_level = HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN); /* 直接读取 PA4，区分引脚变化与消抖事件。 */
    if (current_retest_level != previous_retest_level) /* 仅在 PA4 读数变化时更新边沿记录。 */
    {
        previous_retest_level = current_retest_level; /* 保存新电平，避免长按时重复报告。 */
        if (current_retest_level == GPIO_PIN_RESET) /* 按下 SW8 应让 PA4 变低。 */
        {
            DivControl_Trace("RETEST_PIN_LOW"); /* 证明主循环读到了复测输入的低电平。 */
        }
    }
    if (HAL_GetTick() - previous_trace_ms >= RETEST_TRACE_INTERVAL_MILLISECONDS) /* 每隔一秒确认主循环和 HAL 时钟都仍在推进。 */
    {
        previous_trace_ms = HAL_GetTick(); /* 从本次报告开始计时，限制串口诊断频率。 */
        DivControl_Trace("MAIN_LOOP_ALIVE"); /* 若启动 STATE 后始终没有此行，应检查停机或 UART 错误。 */
    }
    if (DivPower_StopIsActive()) /* 停止按钮优先于任何复测操作。 */
    {
        DivEmergencyStop(); /* 锁存停止并确保两路关闭。 */
        detection_pending = 0; /* 取消未执行的自动检测。 */
        if (!stop_reported) /* 同一次停止只报告一次。 */
        {
            SendStopEvent(); /* 让终端看到停止后供电关闭。 */
            stop_reported = 1; /* 后续循环不重复发送。 */
        }
    }
    if (RetestWasPressed() && HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_SET) /* 停止按钮松开后，新的复测按下事件才有效。 */
    {
        uint32_t previous_interrupt_mask = __get_PRIMASK(); /* 保存进入临界区前的中断状态。 */
        __disable_irq(); /* 防止解除旧停止标志时丢失同时出现的新停止。 */
        if (HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_SET) /* 在临界区内再次确认停止按钮没有按下。 */
        {
            DivPower_ClearStopRequest(); /* 只通过用户明确的复测动作解除停止锁存。 */
            detection_pending = 1; /* 安排新一轮采样和判别，供电动作由模式选择。 */
            stop_reported = 0; /* 允许下一次独立停止事件再次报告。 */
        }
        __set_PRIMASK(previous_interrupt_mask); /* 新挂起的停止中断随后仍可取消这一轮。 */
        if (detection_pending && !DivPower_StopIsActive()) /* 仅在复测请求仍有效时报告已被接受。 */
        {
            DivControl_Trace("RETEST_ACCEPTED"); /* 区分 PA4 电平变化和真正安排新检测。 */
        }
    }
#if AUTO_MEASURE_INTERVAL_MILLISECONDS > 0 /* 只在明确启用周期检测时编译定时判断。 */
    if (!DivPower_StopIsActive() && HAL_GetTick() - last_auto_measure_ms >= AUTO_MEASURE_INTERVAL_MILLISECONDS) /* 到达自动周期时安排新一轮。 */
    {
        detection_pending = 1; /* 定时到期后重新读ADC，不重复打印上轮缓存。 */
    }
#endif /* 按需复测时不产生周期检测，也不进行无意义的零值比较。 */
    if (detection_pending && !DivPower_StopIsActive()) /* 只有有检测请求且未停止才进行 ADC 检测。 */
    {
        detection_pending = 0; /* 执行前先清除请求，防止同一圈重复测量。 */
        DivControl_Trace("DETECTION_START"); /* 证明已经进入本轮供电与采样流程，包括启动测量。 */
        DivMeasurement_Run(); /* 按所选模式采样并发出一条完整报告。 */
#if AUTO_MEASURE_INTERVAL_MILLISECONDS > 0 /* 周期检测模式需要记录发送完成时刻。 */
        last_auto_measure_ms = HAL_GetTick(); /* 从发送完成时刻开始计算下一次检测间隔。 */
#endif /* 按需复测模式仅等待下一次按钮事件。 */
    }
}

