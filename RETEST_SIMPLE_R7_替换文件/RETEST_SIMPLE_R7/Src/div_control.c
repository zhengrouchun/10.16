/* RETEST_SIMPLE_R7: 参考“07-外部中断”例程，把 PA4 简化为“EXTI -> 一个事件标志 -> 主循环直接检测”。 */
#include "main.h"
#include "usart.h"
#include "div_config.h"
#include "div_power.h"
#include "div_control.h"
#include "div_measurement.h"
#include <stdio.h>

/* RETEST 只保留一个待处理事件；STOP 仍由 div_power.c 立即锁存并断电。 */
static volatile uint8_t retest_event_pending = 0U;
static volatile uint8_t buttons_ready = 0U;
static volatile uint32_t retest_raw_irq_count = 0U;
static volatile uint32_t retest_accept_count = 0U;
static volatile uint32_t last_retest_irq_ms = 0U;
static volatile uint8_t retest_irq_seen = 0U;
static int stop_reported = 0;

void DivControl_InitializeButtons(void)
{
    retest_event_pending = 0U;
    retest_raw_irq_count = 0U;
    retest_accept_count = 0U;
    last_retest_irq_ms = 0U;
    retest_irq_seen = 0U;
    stop_reported = 0;
    buttons_ready = 1U;
}

/*
 * PA4 EXTI4 下降沿回调。
 * 与跑通的“07-外部中断”例程保持同一思路：中断里只改一个标志。
 * 不在中断里做 ADC、OLED、串口，也不再次读取 PA4。
 */
void DivControl_OnRetestInterrupt(void)
{
    uint32_t now_ms;

    ++retest_raw_irq_count;

    if (!buttons_ready)
    {
        return;
    }

    /* STOP 物理按住时，RETEST 不允许抢占安全停机。 */
    if (HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_RESET)
    {
        return;
    }

    now_ms = HAL_GetTick();

    /* 50 ms 简单防抖：一次真实按键只交付一个事件。 */
    if (retest_irq_seen && (uint32_t)(now_ms - last_retest_irq_ms) < 50U)
    {
        return;
    }

    retest_irq_seen = 1U;
    last_retest_irq_ms = now_ms;
    retest_event_pending = 1U;
    ++retest_accept_count;
}

/* 保留旧接口，EXTI4 方案不再使用 SysTick 扫描。 */
void DivButtonsTick(void)
{
}

static int RetestTakeEvent(void)
{
    uint32_t saved_mask;
    int pressed;

    saved_mask = __get_PRIMASK();
    __disable_irq();
    pressed = (retest_event_pending != 0U);
    retest_event_pending = 0U;
    __set_PRIMASK(saved_mask);

    return pressed;
}

void DivControl_Trace(const char *event)
{
    static char line[BUTTON_TRACE_BUFFER_SIZE];
    uint32_t input_snapshot = GPIOA->IDR;
    int written;

    written = snprintf(line, sizeof(line),
        "EV=%s T=%lu PA4=%s PA5=%s PEND=%s STOP=%s IDR=0x%04lX RAW_IRQ=%lu ACCEPT=%lu FW=RETEST_SIMPLE_R7\r\n",
        event,
        (unsigned long)HAL_GetTick(),
        (input_snapshot & RETEST_BUTTON_PIN) ? "HIGH" : "LOW",
        (input_snapshot & STOP_BUTTON_PIN) ? "HIGH" : "LOW",
        retest_event_pending ? "YES" : "NO",
        DivPower_StopIsActive() ? "YES" : "NO",
        (unsigned long)input_snapshot,
        (unsigned long)retest_raw_irq_count,
        (unsigned long)retest_accept_count);

    if (written <= 0 || written >= (int)sizeof(line))
    {
        Error_Handler();
    }

    if (HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)written,
                          UART_TIMEOUT_MILLISECONDS) != HAL_OK)
    {
        Error_Handler();
    }
}

static void SendStopEvent(void)
{
#if DIV_REFERENCE_SERIAL_MODE
    const char stopped[] = "EVENT=STOP RUN_POWER=NOT_IMPLEMENTED MODE=WAIT_RETEST\r\n";
#else
    const char stopped[] = "EVENT=STOP RUN_POWER=OFF TEST_POWER=OFF MODE=WAIT_RETEST\r\n";
#endif

    if (HAL_UART_Transmit(&huart1, (uint8_t *)stopped,
                          (uint16_t)(sizeof(stopped) - 1U),
                          UART_TIMEOUT_MILLISECONDS) != HAL_OK)
    {
        Error_Handler();
    }
}

/*
 * 上电检测不再依赖 detection_pending。
 * OLED 初始化完成后直接执行一次完整检测，避免出现长期 DIV READY / STATE=WAITING。
 */
void DivControl_RunInitialDetection(void)
{
    if (HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_RESET)
    {
        DivEmergencyStop();
        return;
    }

    if (DivPower_StopIsActive())
    {
        return;
    }

    DivControl_Trace("DETECTION_START");
    DivMeasurement_Run();
}

/*
 * 主循环调度：
 * 1) STOP 物理按下 -> 立即停机；
 * 2) RETEST 事件 -> 清 STOP 锁存 -> 直接检测；
 * 3) STOP 已锁存但按钮已松开 -> 保持停机等待 RETEST。
 * 不再使用 detection_pending、SEEN_RELEASE、LOW_STREAK 等多层状态。
 */
void DivControl_Service(void)
{
    int retest_pressed;

    /* STOP 物理按住时永远最高优先级。 */
    if (HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_RESET)
    {
        DivEmergencyStop();
        if (!stop_reported)
        {
            SendStopEvent();
            stop_reported = 1;
        }
        return;
    }

    retest_pressed = RetestTakeEvent();

    if (retest_pressed)
    {
        uint32_t saved_mask;
        int can_retest = 0;

        /* 防止正在解除 STOP 锁存时恰好又按下 STOP。 */
        saved_mask = __get_PRIMASK();
        __disable_irq();
        if (HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_SET)
        {
            DivPower_ClearStopRequest();
            can_retest = 1;
            stop_reported = 0;
        }
        __set_PRIMASK(saved_mask);

        if (can_retest && !DivPower_StopIsActive())
        {
            DivControl_Trace("RETEST_ACCEPTED");
            DivControl_Trace("DETECTION_START");
            DivMeasurement_Run();
        }
        return;
    }

    /* STOP 已经锁存但按钮已经松开：保持断电，等下一次 RETEST。 */
    if (DivPower_StopIsActive())
    {
        if (!stop_reported)
        {
            SendStopEvent();
            stop_reported = 1;
        }
        return;
    }
}
