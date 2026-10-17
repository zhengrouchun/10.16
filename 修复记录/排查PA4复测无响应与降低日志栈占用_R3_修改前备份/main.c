/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h> /* 将真实读数格式化为串口文本。 */
#include <string.h> /* 使用明确的字符串比较检查 ADC 返回状态。 */
#include "oled_display.h" /* 使用新增SSD1306显示驱动。 */
#include "div_diagnosis.h" /* 引入参数 2.0 的电压换算和九工况判别。 */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct /* 保存一次检测结果，初始化时所有有效标志为假。 */
{
    uint32_t vb_raw; /* VB 的真实 ADC 原码，仅在 vb_valid 为真时可用。 */
    uint32_t va_raw; /* VA 的真实 ADC 原码，仅在 va_valid 为真时可用。 */
    uint32_t vb_mv; /* VB 四舍五入后的 mV 值。 */
    uint32_t va_mv; /* VA 四舍五入后的 mV 值。 */
    uint32_t adc_conversions; /* 每次 Poll 成功并 GetValue 后递增。 */
    uint32_t feature_reads; /* 实际获得有效读数的不同节点数。 */
    uint32_t elapsed_ms; /* 截图模式只统计两点采样和判别耗时，不含串口发送及报告间隔。 */
    int vb_valid; /* VB 已有效采样时为真，未读时输出 NA。 */
    int va_valid; /* VA 已有效采样时为真，未读时输出 NA。 */
    int state; /* 唯一工况编号，或 DIV_UNKNOWN。 */
    const char *error; /* OK 或明确的 ADC 错误阶段。 */
} DivMeasurement; /* 为本次采集记录定义类型名。 */

typedef struct /* 保存复测按钮的消抖状态，持续按下只触发一次。 */
{
    GPIO_PinState raw_level; /* 最近一次读到的按钮电平。 */
    GPIO_PinState stable_level; /* 已连续稳定足够时间的按钮电平。 */
    uint32_t changed_ms; /* 原始电平最后变化的时刻。 */
} RetestButton; /* 为复测按钮的状态定义类型名。 */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define DIV_REFERENCE_SERIAL_MODE 0 /* 1=PB12使能1V测试并按截图连续报告，不切3V；0=原正常工况切3V流程。 */
#define ADC_TIMEOUT_MILLISECONDS 20 /* 最多等待一次 ADC 转换 20 ms。 */
#define UART_TIMEOUT_MILLISECONDS 1000 /* 一条报告最多等待1000ms；9600波特发送约300字节需要约313ms。 */
#define REPORT_BUFFER_SIZE 384 /* 容纳整条带状态和错误信息的报告。 */
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

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

static volatile int power_gpio_ready = 0; /* GPIO 尚未初始化时禁止访问断电输出。 */
static volatile int stop_requested = 0; /* 停止中断锁存此标志，只有松开停止并按复测才解除。 */
static int detection_pending = 1; /* 上电安排一次检测，完成后等待复测按钮。 */
static int stop_reported = 0; /* 一次停止事件只输出一条事件报告。 */
static RetestButton retest_button = {GPIO_PIN_SET, GPIO_PIN_SET, 0}; /* 未按按钮默认为高电平。 */

static volatile int buttons_ready = 0; /* 按键状态初始化完成后才允许SysTick消抖。 */
static volatile int retest_event_pending = 0; /* 锁存短按，串口和OLED忙碌期间也不会丢失。 */
static volatile int retest_armed = 1; /* 停止后必须先确认复测按钮松开，才允许下一次按下。 */
#if OLED_DISPLAY_ENABLE /* 启用 OLED 时保存测量结果及画面刷新请求。 */
static volatile int display_dirty = 1; /* 主循环负责刷新，停止中断只请求刷新。 */
static DivMeasurement last_measurement; /* 保存最近一次1V测试结果，运行时不重新采样。 */
static int has_measurement = 0; /* 尚未采样时显示NA而不是伪造零电压。 */
static int oled_reported_init_state = -1; /* 首次进入主循环时报告参考驱动是否已完成初始化。 */
#endif /* 显示开关结束。 */
#if OLED_DISPLAY_ENABLE /* OLED关闭时不再发送调试心跳。 */
static uint32_t last_status_ms = 0; /* 保存上一条定时状态报告的起始毫秒。 */
#endif /* 结束OLED心跳计时变量。 */
#if AUTO_MEASURE_INTERVAL_MILLISECONDS > 0 /* 只有启用定时复测时才保存上次检测时刻。 */
static uint32_t last_auto_measure_ms = 0; /* 保存上一条报告发送完毕的时刻，用于安排下一轮。 */
#endif /* 按需复测模式不需要定时器状态。 */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
 void MX_ADC1_Init(void); /* 手动恢复：ADC1初始化函数声明。 */
static const char *ReadAdcChannel(uint32_t channel, uint32_t *raw, uint32_t *conversions); /* 声明真实采样函数。 */
static void SendReport(const DivMeasurement *result); /* 声明报告函数。 */
static void RunDivDetection(void); /* 声明一次完整检测。 */
static void AllPowerOff(void); /* 声明无等待的双路断电函数，可由停止中断调用。 */
static void ServiceControls(void); /* 声明复测和停止按钮的处理函数。 */
#if OLED_DISPLAY_ENABLE /* 暂停OLED时不编译显示调试入口。 */
static void ServiceStatus(void); /* 声明不会重新采样ADC的定时运行诊断。 */
#endif /* 结束显示调试声明。 */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void AllPowerOff(void) /* 同一次 GPIO 写操作关闭两个开关，不在中断里延时。 */
{
    if (power_gpio_ready) /* 初始化未完成时由外部下拉电阻维持断电。 */
    {
        HAL_GPIO_WritePin(GPIOB, TEST_POWER_PIN | RUN_POWER_PIN, GPIO_PIN_RESET); /* 将 PB12 和 PB13 一起拉低。 */
    }
}

static int StopIsActive(void) /* 同时检查锁存事件和按钮实时电平，包含上电时已按住的情况。 */
{
    return stop_requested || HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_RESET; /* 任一条件成立就禁止上电。 */
}

void DivEmergencyStop(void) /* 供停止中断和故障中断使用，不调用串口或 HAL_Delay。 */
{
    if (!stop_requested) /* 只在新停止到来时丢弃旧复测请求。 */
    {
        retest_event_pending = 0; /* 停止之前排队的复测不得使机器重新上电。 */
        retest_armed = 0; /* 停止到来时正在消抖的旧按下也必须作废。 */
#if OLED_DISPLAY_ENABLE /* OLED暂停时仅保留停止控制。 */
        display_dirty = 1; /* 请求主循环显示STOPPED。 */
#endif /* 结束停止画面请求。 */
    }
    stop_requested = 1; /* 锁存停止，按钮松开也不会自动恢复运行。 */
    AllPowerOff(); /* 立即关闭测试和运行电源。 */
}

void HAL_GPIO_EXTI_Callback(uint16_t pin) /* HAL 在处理停止按钮的下降沿中断后调用此函数。 */
{
    if (pin == STOP_BUTTON_PIN) /* 只处理本工程接在 PA5 的停止按钮。 */
    {
        DivEmergencyStop(); /* 按下时立即断电，主循环随后发送停止事件。 */
    }
}

static int WaitWithoutStop(uint32_t milliseconds) /* 在稳定等待和互锁等待期间也允许停止取消检测。 */
{
    uint32_t started_ms = HAL_GetTick(); /* 保存等待开始时刻。 */
    while (HAL_GetTick() - started_ms < milliseconds) /* 无符号差值允许毫秒计数器回绕。 */
    {
        if (StopIsActive()) /* 检查等待期间是否出现停止。 */
        {
            DivEmergencyStop(); /* 即使按钮没有产生新下降沿，也保证立即断电。 */
            return 0; /* 告诉调用方本次等待被停止取消。 */
        }
        HAL_Delay(CONTROL_POLL_MILLISECONDS); /* 等待一个毫秒，保持 SysTick 正常更新。 */
    }
    if (StopIsActive()) /* 在等待结束的边界再检查一次停止。 */
    {
        DivEmergencyStop(); /* 保证边界出现的停止也关闭两路。 */
        return 0; /* 禁止继续合闸。 */
    }
    return 1; /* 等待完成且没有停止请求。 */
}

static int SelectPower(uint16_t selected_pin) /* 只允许在两个开关均关闭并等待之后开启其中一路。 */
{
    uint32_t previous_interrupt_mask; /* 保存调用前的中断屏蔽状态，不能擅自开启原本关闭的中断。 */
    int enabled = 0; /* 默认没有合闸，只有通过所有检查才置为成功。 */
    AllPowerOff(); /* 每次切换首先断开旧电源和另一条电源。 */
    if (selected_pin != TEST_POWER_PIN && selected_pin != RUN_POWER_PIN) /* 拒绝同时开启两路或传入其他引脚。 */
    {
        return 0; /* 保持双路关闭并报告未合闸。 */
    }
    if (!WaitWithoutStop(POWER_BREAK_MILLISECONDS)) /* 至少保持两路断开 5 ms，并接受停止取消。 */
    {
        return 0; /* 被停止后不允许合闸。 */
    }
    previous_interrupt_mask = __get_PRIMASK(); /* 保存当前中断状态。 */
    __disable_irq(); /* 将停止检查和单路合闸作为不可被打断的一组操作。 */
    if (!StopIsActive()) /* 若停止已发生或按钮仍按住，就维持断电。 */
    {
        HAL_GPIO_WritePin(GPIOB, selected_pin, GPIO_PIN_SET); /* 只拉高选中的一根控制线。 */
        enabled = 1; /* 记录已经发出单路开启命令。 */
    }
    __set_PRIMASK(previous_interrupt_mask); /* 恢复原中断状态，已挂起的停止中断将立即关闭电源。 */
    return enabled; /* 后续步骤仍会检查停止标志，避免接受已被取消的检测。 */
}


void DivButtonsTick(void) /* SysTick每毫秒调用，只读按键和更新标志，不等待、不发送数据。 */
{
    GPIO_PinState current_level; /* 保存本次复测输入电平。 */
    uint32_t now_ms; /* 保存消抖使用的时间。 */
    if (!buttons_ready) /* GPIO和初始按键状态未就绪时不访问输入。 */
    {
        return; /* 保持外部下拉所保证的默认断电。 */
    }
    current_level = HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN); /* 读取PA4，按下为低。 */
    now_ms = HAL_GetTick(); /* 获取已经递增的毫秒计数。 */
    if (HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_RESET) /* 停止保持按下时丢弃复测操作。 */
    {
        retest_armed = 0; /* 需要在停止松开后重新确认复测按钮释放。 */
        retest_event_pending = 0; /* 两个按钮同时按下时停止优先。 */
    }
    if (current_level != retest_button.raw_level) /* 原始电平变化时重新计时。 */
    {
        retest_button.raw_level = current_level; /* 保存原始输入。 */
        retest_button.changed_ms = now_ms; /* 从此时开始观察连续稳定时间。 */
    }
    if (current_level != retest_button.stable_level && now_ms - retest_button.changed_ms >= BUTTON_DEBOUNCE_MILLISECONDS) /* 电平稳定20ms才接受。 */
    {
        retest_button.stable_level = current_level; /* 同时记录按下和松开，避免长按重复。 */
        if (current_level == GPIO_PIN_RESET && retest_armed && HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_SET) /* 只接受已经重新允许的独立复测按下。 */
        {
            retest_event_pending = 1; /* 即使主循环正在刷新屏幕，也保存这次复测。 */
            retest_armed = 0; /* 当前按下只允许触发一次，等待下一次稳定松开。 */
        }
    }
    if (current_level == GPIO_PIN_SET && retest_button.stable_level == GPIO_PIN_SET && now_ms - retest_button.changed_ms >= BUTTON_DEBOUNCE_MILLISECONDS && HAL_GPIO_ReadPin(GPIOA, STOP_BUTTON_PIN) == GPIO_PIN_SET) /* 两键松开且复测高电平稳定后才重新允许按下。 */
    {
        retest_armed = 1; /* 此后的独立按下才能解除停止锁存。 */
    }
}
static int RetestWasPressed(void) /* 主循环原子地取走一次已经消抖的复测事件。 */
{
    uint32_t saved_mask = __get_PRIMASK(); /* 保存调用前的中断屏蔽状态。 */
    int pressed; /* 保存要交给主循环的事件。 */
    __disable_irq(); /* 防止读取与清除之间丢失新的事件。 */
    pressed = retest_event_pending; /* 读取已锁存的事件。 */
    retest_event_pending = 0; /* 一次事件只交付一次。 */
    __set_PRIMASK(saved_mask); /* 立即恢复中断，让停止中断保持及时。 */
    return pressed; /* 返回是否需要执行新检测。 */
}

static const char *ReadAdcChannel(uint32_t channel, uint32_t *raw, uint32_t *conversions) /* 失败返回阶段名，成功返回 OK。 */
{
    ADC_ChannelConfTypeDef selected = {0}; /* 全部配置字段清零，随后明确设置所需字段。 */
    if (StopIsActive()) /* 停止若发生在上一通道转换期间，不再启动下一通道。 */
    {
        return "STOP_REQUESTED"; /* 断电后的读数不能作为新的有效测试采样。 */
    }
    selected.Channel = channel; /* 选择 PA0 的通道 0 或 PA1 的通道 1。 */
    selected.Rank = ADC_REGULAR_RANK_1; /* 单次转换只使用序列第一个位置。 */
    selected.SamplingTime = ADC_SAMPLETIME_239CYCLES_5; /* 给分压节点 239.5 个 ADC 周期采样时间。 */
    if (HAL_ADC_ConfigChannel(&hadc1, &selected) != HAL_OK) /* 在 ADC 停止期间切换通道。 */
    {
        return "ADC_CONFIG_FAILED"; /* 配置失败，没有启动转换。 */
    }
    if (StopIsActive()) /* 配置完成到启动之前再次检查停止。 */
    {
        return "STOP_REQUESTED"; /* 不在已经断电的电路上启动 ADC 转换。 */
    }
    if (HAL_ADC_Start(&hadc1) != HAL_OK) /* 软件触发一次真实转换。 */
    {
        if (HAL_ADC_Stop(&hadc1) != HAL_OK) /* 启动失败也尝试让 ADC 回到停止状态。 */
        {
            return "ADC_START_AND_STOP_FAILED"; /* 同时报告启动与清理失败。 */
        }
        return "ADC_START_FAILED"; /* 不把启动失败记作完成转换。 */
    }
    if (HAL_ADC_PollForConversion(&hadc1, ADC_TIMEOUT_MILLISECONDS) != HAL_OK) /* 等待真实完成标志，最多 20 ms。 */
    {
        if (HAL_ADC_Stop(&hadc1) != HAL_OK) /* 超时或错误后也关闭 ADC。 */
        {
            return "ADC_POLL_AND_STOP_FAILED"; /* 明确报告等待和停止均失败。 */
        }
        return "ADC_POLL_FAILED"; /* 未确认完成的转换不计入成功转换数。 */
    }
    *raw = HAL_ADC_GetValue(&hadc1); /* 唯一的原码来源：读取 ADC 数据寄存器。 */
    ++(*conversions); /* 确认完成并取值后记录一次，停止失败也不会抹去已经发生的转换。 */
    if (HAL_ADC_Stop(&hadc1) != HAL_OK) /* 每次取值后停止，再允许下一次切换通道。 */
    {
        return "ADC_STOP_FAILED"; /* ADC 状态不可靠，本轮不接受为正常测量。 */
    }
    if (*raw > DIV_ADC_MAX_CODE) /* 检查返回值与 12 位右对齐配置一致。 */
    {
        return "ADC_RANGE_FAILED"; /* 不对越界原码套用电压换算。 */
    }
    return "OK"; /* 本次配置、启动、等待、取值和停止全部成功。 */
}

static void SendReport(const DivMeasurement *result) /* 只发送已经保存的检测结果，不再进行 ADC 转换。 */
{
    char line[REPORT_BUFFER_SIZE]; /* 保存完整的一行串口报告。 */
    char state_text[16] = "UNKNOWN"; /* 未确认唯一匹配前使用未知状态。 */
    char vb_raw_text[12] = "NA"; /* 未取得有效 VB 原码时不发送伪造的零。 */
    char va_raw_text[12] = "NA"; /* 未取得有效 VA 原码时显示未读。 */
    char vb_mv_text[12] = "NA"; /* VB 未读时没有可用的毫伏值。 */
    char va_mv_text[12] = "NA"; /* VA 未读时没有可用的毫伏值。 */
    const char *kind = "UNKNOWN"; /* 将正常、已知故障、未知分开显示。 */
    int written; /* 保存 snprintf 实际需要的字符数。 */
#if OLED_DISPLAY_ENABLE /* OLED暂停时不复制显示专用结果。 */
    last_measurement = *result; /* 保存本轮1V测试结果供屏幕使用。 */
    has_measurement = 1; /* 本轮报告已包含真实采样有效性信息。 */
    display_dirty = 1; /* 测量完成后请求更新画面。 */
#endif /* 结束显示缓存更新。 */
    if (result->state >= 0) /* 只有唯一匹配才输出 DIV 编号。 */
    {
        snprintf(state_text, sizeof(state_text), "DIV%d", result->state); /* 将内部编号转换为 DIV0 至 DIV8。 */
        kind = result->state == DIV_NORMAL_CASE ? "NORMAL" : "FAULT"; /* 正常编号使用具名常量表达。 */
    }
    if (result->vb_valid) /* 只有有效 VB 采样才能进入报告。 */
    {
        snprintf(vb_raw_text, sizeof(vb_raw_text), "%lu", (unsigned long)result->vb_raw); /* 格式化真实 VB 原码。 */
        snprintf(vb_mv_text, sizeof(vb_mv_text), "%lu", (unsigned long)result->vb_mv); /* 格式化 VB 换算值。 */
    }
    if (result->va_valid) /* 独立判断 VA，避免沿用上一轮数据。 */
    {
        snprintf(va_raw_text, sizeof(va_raw_text), "%lu", (unsigned long)result->va_raw); /* 格式化真实 VA 原码。 */
        snprintf(va_mv_text, sizeof(va_mv_text), "%lu", (unsigned long)result->va_mv); /* 格式化 VA 换算值。 */
    }
#if DIV_REFERENCE_SERIAL_MODE /* 字段名称、顺序及换行与参考工程和截图保持一致。 */
    written = snprintf(line, sizeof(line), /* 将本轮实际测量值组织成一行。 */
        "STATE=%s KIND=%s VB_RAW=%s VB_MV=%s VA_RAW=%s VA_MV=%s " /* 状态、类型、VB和VA原码及毫伏值。 */
        "FEATURE_READS=%lu ADC_CONVERSIONS=%lu ELAPSED_MS=%lu " /* 有效节点数、成功转换数、检测耗时。 */
        "RUN_POWER=NOT_IMPLEMENTED ERROR=%s\r\n", /* 此模式只使能1V测试，不实现3V运行供电；格式与图二一致。 */
        state_text, kind, vb_raw_text, vb_mv_text, va_raw_text, va_mv_text, /* 依次填入前六个文本字段。 */
        (unsigned long)result->feature_reads, (unsigned long)result->adc_conversions, /* 计数来自当前这轮采样。 */
        (unsigned long)result->elapsed_ms, result->error); /* 耗时和错误来自当前这轮检测。 */
#else /* 保留原受控电源模式的真实控制脚报告。 */
    written = snprintf(line, sizeof(line), /* 按固定字段名组成便于保存与核对的串口记录。 */
        "STATE=%s KIND=%s VB_RAW=%s VB_MV=%s VA_RAW=%s VA_MV=%s " /* 先输出状态和两点原码、电压。 */
        "FEATURE_READS=%lu ADC_CONVERSIONS=%lu ELAPSED_MS=%lu " /* 分别输出节点数、转换数和检测耗时。 */
        "RUN_POWER=%s TEST_POWER=%s POWER_SOURCE=GPIO_LEVEL ERROR=%s\r\n", /* GPIO 电平是控制证据，不能冒充独立电源测量。 */
        state_text, kind, vb_raw_text, vb_mv_text, va_raw_text, va_mv_text, /* 对应前六个文本字段。 */
        (unsigned long)result->feature_reads, (unsigned long)result->adc_conversions, /* 使用本轮实际累加的计数。 */
        (unsigned long)result->elapsed_ms, /* 输出包含测试准备和供电切换的检测耗时。 */
        HAL_GPIO_ReadPin(GPIOB, RUN_POWER_PIN) == GPIO_PIN_SET ? "ON" : "OFF", /* 读取 PB13 的实际逻辑电平快照。 */
        HAL_GPIO_ReadPin(GPIOB, TEST_POWER_PIN) == GPIO_PIN_SET ? "ON" : "OFF", /* 读取 PB12 的实际逻辑电平快照。 */
        result->error); /* 输出本轮的 ADC 错误或停止原因。 */
#endif /* 结束串口报告格式选择。 */
    if (written < 0 || written >= (int)sizeof(line)) /* 检查格式化失败和缓冲区不足。 */
    {
        Error_Handler(); /* 不发送截断或损坏的记录。 */
    }
    if (HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)written, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 由 PA9 发往终端 RXD。 */
    {
        Error_Handler(); /* 串口发送失败时停止，避免伪称已交付数据。 */
    }
}

static void RunDivDetection(void) /* 每轮重新采集VB、VA，再换算、判别并输出一条报告。 */
{
    DivMeasurement result = {0}; /* 清空这一轮记录；两个有效标志均为假。 */
    uint32_t started_ms = HAL_GetTick(); /* 先记录准备开始时刻；截图模式在电源稳定后重置采样计时。 */
    result.state = DIV_UNKNOWN; /* 在读完两点并成功判别之前维持未知。 */
    result.error = "STOP_REQUESTED"; /* 建立测试状态失败时报告停止，不能伪造 ADC 数据。 */
    if (!SelectPower(TEST_POWER_PIN) || !WaitWithoutStop(TEST_POWER_OBSERVATION_MILLISECONDS)) /* 关闭运行、互锁等待、打开测试并保持1.5秒供观察。 */
    {
        AllPowerOff(); /* 建立测试状态失败时保证双路关闭。 */
        result.elapsed_ms = HAL_GetTick() - started_ms; /* 保存到取消时刻的真实耗时。 */
        SendReport(&result); /* 未采样的电压输出 NA，次数保持零。 */
        return; /* 本次被取消，等待新的复测事件。 */
    }
#if DIV_REFERENCE_SERIAL_MODE /* 串口沿用参考工程的采样计时口径，不计入建立1V测试条件的等待。 */
    started_ms = HAL_GetTick(); /* PB12已使能且稳定20ms后，从真正准备采样时开始计时。 */
#endif /* 原受控运行模式仍保留包含电源切换的总耗时。 */
    result.error = ReadAdcChannel(ADC_CHANNEL_0, &result.vb_raw, &result.adc_conversions); /* 从 PA0 读取 VB。 */
    if (strcmp(result.error, "OK") == 0) /* 明确比较整个返回字符串，确认 VB 采样成功。 */
    {
        result.vb_valid = 1; /* 标记本轮 VB 已读取。 */
        result.vb_mv = DivCodeToMillivolts(result.vb_raw); /* 由原码换算 VB 的毫伏值。 */
        ++result.feature_reads; /* VB 是本轮第一个有效节点。 */
        result.error = ReadAdcChannel(ADC_CHANNEL_1, &result.va_raw, &result.adc_conversions); /* 切到 PA1 读取 VA。 */
        if (strcmp(result.error, "OK") == 0) /* VA 采样成功才允许两点判别。 */
        {
            result.va_valid = 1; /* 标记本轮 VA 已读取。 */
            result.va_mv = DivCodeToMillivolts(result.va_raw); /* 由原码换算 VA 的毫伏值。 */
            ++result.feature_reads; /* VA 是本轮第二个有效节点。 */
            result.state = DivClassify((int32_t)result.vb_mv, (int32_t)result.va_mv); /* 用电压闭区间确定唯一工况。 */
        }
    }
    if (StopIsActive()) /* 即使停止发生在 ADC 转换期间，也禁止接受结果并开启运行。 */
    {
        result.state = DIV_UNKNOWN; /* 已取消的检测不作为正常或故障验收结果。 */
        result.error = "STOP_REQUESTED"; /* 报告本轮取消原因。 */
        AllPowerOff(); /* 保持停止时的双路关闭。 */
    }
#if !DIV_REFERENCE_SERIAL_MODE /* 截图模式不自动切到3V，避免下一轮采到运行态电压。 */
    else if (strcmp(result.error, "OK") == 0 && result.vb_valid && result.va_valid && result.state == DIV_NORMAL_CASE) /* 两点均成功且唯一匹配正常才进入运行。 */
    {
        if (!SelectPower(RUN_POWER_PIN) || StopIsActive()) /* 先断开测试电源，再等待 5 ms，最后尝试开启运行。 */
        {
            result.state = DIV_UNKNOWN; /* 切换被停止取消时拒绝正常结果。 */
            result.error = "STOP_REQUESTED"; /* 输出明确的取消原因。 */
            AllPowerOff(); /* 保证双路最终关闭。 */
        }
    }
    else /* 已知故障、未知工况或任何 ADC 失败都不允许进入运行。 */
    {
        AllPowerOff(); /* 检测完成后连测试电源也关闭，保持等待复测状态。 */
    }
#else /* 连续串口测试模式完成本轮后关闭测试使能，下一轮重新建立1V条件。 */
    AllPowerOff(); /* 将PB12和PB13拉低；图中手动SW1/SW7仍需用户实际操作。 */
#endif /* 结束原受控电源模式的运行切换。 */
    result.elapsed_ms = HAL_GetTick() - started_ms; /* 记录真实耗时，不计串口发送和500ms等待，不固定为截图中的2ms。 */
    SendReport(&result); /* 发送刚采集到的数据，下一轮会重新读取ADC。 */
#if DIV_REFERENCE_SERIAL_MODE /* 与参考代码一致，ADC错误报告后停止；按键停止仍允许PA4恢复。 */
    if (strcmp(result.error, "OK") != 0 && strcmp(result.error, "STOP_REQUESTED") != 0) /* 区分ADC失败和用户主动停止。 */
    {
        Error_Handler(); /* ADC失败时保留故障现场，复位后重新校准和测量。 */
    }
#endif /* 结束截图模式的ADC故障处理。 */
}

static void SendRetestTrace(const char *event) /* 通过独立事件行指出复测流程实际到达的位置。 */
{
    char line[320]; /* 容纳两个按键的寄存器配置、电平及本次固件标识。 */
    uint32_t input_snapshot = GPIOA->IDR; /* 直接保存 GPIOA 输入寄存器，用于和外部电压及 HAL 读数对照。 */
    uint32_t config_snapshot = GPIOA->CRL; /* 保存 PA0 至 PA7 的实际配置，检查 PA4 是否被其他初始化覆盖。 */
    uint32_t retest_config = (config_snapshot & (GPIO_CRL_MODE4 | GPIO_CRL_CNF4)) >> GPIO_CRL_MODE4_Pos; /* 提取 PA4 的四位配置；使用芯片头文件提供的位名，避免硬写移位值。 */
    uint32_t stop_config = (config_snapshot & (GPIO_CRL_MODE5 | GPIO_CRL_CNF5)) >> GPIO_CRL_MODE5_Pos; /* 从同一次配置快照中提取 PA5 模式，用于核对 CubeMX 配置是否生效。 */
    int written = snprintf(line, sizeof(line), /* 将一次输入快照和独立 HAL 读取组成完整诊断记录。 */
        "EVENT=%s TICK_MS=%lu PA4=%s PA5=%s ARMED=%s PENDING=%s STOP_LATCH=%s GPIOA_IDR=0x%04lX PA4_CFG=0x%lX PA5_CFG=0x%lX HAL_PA4=%s FW=OLED_STOP_R2\r\n", /* 标识本次构建；上下拉模式以实际寄存器为准，不再固定声称使用外部上拉。 */
        event, (unsigned long)HAL_GetTick(), /* 填入事件名和当前毫秒计时。 */
        (input_snapshot & RETEST_BUTTON_PIN) != 0 ? "HIGH" : "LOW", /* 用输入快照的 PA4 位报告复测电平，零表示低电平。 */
        (input_snapshot & STOP_BUTTON_PIN) != 0 ? "HIGH" : "LOW", /* 用同一次快照的 PA5 位报告停止电平。 */
        retest_armed ? "YES" : "NO", retest_event_pending ? "YES" : "NO", stop_requested ? "YES" : "NO", /* 填入允许复测、排队事件和停止锁存状态。 */
        (unsigned long)input_snapshot, (unsigned long)retest_config, (unsigned long)stop_config, /* 输出原始输入值及 PA4、PA5 的实际配置。 */
        HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN) == GPIO_PIN_SET ? "HIGH" : "LOW"); /* 独立调用 HAL 读取 PA4；保持闭合时应与快照结果一致。 */
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

static void ServiceControls(void) /* 停止优先；按需模式只在启动和有效复测后安排检测。 */
{
    static GPIO_PinState previous_retest_level = GPIO_PIN_SET; /* 保存主循环上次看到的 PA4 电平，用于只报告按下沿。 */
    static uint32_t previous_trace_ms = 0; /* 保存上次主循环存活报告的时刻。 */
    GPIO_PinState current_retest_level = HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN); /* 直接读取 PA4，区分引脚变化与消抖事件。 */
    if (current_retest_level != previous_retest_level) /* 仅在 PA4 读数变化时更新边沿记录。 */
    {
        previous_retest_level = current_retest_level; /* 保存新电平，避免长按时重复报告。 */
        if (current_retest_level == GPIO_PIN_RESET) /* 按下 SW8 应让 PA4 变低。 */
        {
            SendRetestTrace("RETEST_PIN_LOW"); /* 证明主循环读到了复测输入的低电平。 */
        }
    }
    if (HAL_GetTick() - previous_trace_ms >= RETEST_TRACE_INTERVAL_MILLISECONDS) /* 每隔一秒确认主循环和 HAL 时钟都仍在推进。 */
    {
        previous_trace_ms = HAL_GetTick(); /* 从本次报告开始计时，限制串口诊断频率。 */
        SendRetestTrace("MAIN_LOOP_ALIVE"); /* 若启动 STATE 后始终没有此行，应检查停机或 UART 错误。 */
    }
    if (StopIsActive()) /* 停止按钮优先于任何复测操作。 */
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
            stop_requested = 0; /* 只通过用户明确的复测动作解除停止锁存。 */
            detection_pending = 1; /* 安排新一轮采样和判别，供电动作由模式选择。 */
            stop_reported = 0; /* 允许下一次独立停止事件再次报告。 */
        }
        __set_PRIMASK(previous_interrupt_mask); /* 新挂起的停止中断随后仍可取消这一轮。 */
        if (detection_pending && !StopIsActive()) /* 仅在复测请求仍有效时报告已被接受。 */
        {
            SendRetestTrace("RETEST_ACCEPTED"); /* 区分 PA4 电平变化和真正安排新检测。 */
        }
    }
#if AUTO_MEASURE_INTERVAL_MILLISECONDS > 0 /* 只在明确启用周期检测时编译定时判断。 */
    if (!StopIsActive() && HAL_GetTick() - last_auto_measure_ms >= AUTO_MEASURE_INTERVAL_MILLISECONDS) /* 到达自动周期时安排新一轮。 */
    {
        detection_pending = 1; /* 定时到期后重新读ADC，不重复打印上轮缓存。 */
    }
#endif /* 按需复测时不产生周期检测，也不进行无意义的零值比较。 */
    if (detection_pending && !StopIsActive()) /* 只有有检测请求且未停止才进行 ADC 检测。 */
    {
        detection_pending = 0; /* 执行前先清除请求，防止同一圈重复测量。 */
        SendRetestTrace("DETECTION_START"); /* 证明已经进入本轮供电与采样流程，包括启动测量。 */
        RunDivDetection(); /* 按所选模式采样并发出一条完整报告。 */
#if AUTO_MEASURE_INTERVAL_MILLISECONDS > 0 /* 周期检测模式需要记录发送完成时刻。 */
        last_auto_measure_ms = HAL_GetTick(); /* 从发送完成时刻开始计算下一次检测间隔。 */
#endif /* 按需复测模式仅等待下一次按钮事件。 */
    }
}

#if OLED_DISPLAY_ENABLE /* 显示启用时编译状态报告和屏幕刷新流程。 */
static void ServiceStatus(void) /* 每秒发送一次存活与总线状态，持续运行不等于重复进行DIV检测。 */
{
    uint32_t now_ms = HAL_GetTick(); /* 获取当前毫秒时刻，证明主循环仍在运行。 */
    char line[384]; /* 容纳测量状态、控制脚及 OLED 初始化状态。 */
    char measured_state[20] = "WAITING"; /* 首轮测量前标明尚无判别结果。 */
    int written; /* 保存格式化的字符数量并检查是否溢出。 */
    if (now_ms - last_status_ms < STATUS_INTERVAL_MILLISECONDS) /* 每秒最多发送一条，避免串口占用过久。 */
    {
        return; /* 时间未到时继续及时处理按钮和停止。 */
    }
    last_status_ms = now_ms; /* 记录本次报告时刻，毫秒回绕后差值仍有效。 */
    if (StopIsActive()) /* 停止状态比上一次保存的正常测量结果优先。 */
    {
        snprintf(measured_state, sizeof(measured_state), "STOPPED"); /* 明确表示两路应关闭。 */
    }
    else if (has_measurement && strcmp(last_measurement.error, "OK") != 0) /* 区分ADC错误和真实的未知工况。 */
    {
        snprintf(measured_state, sizeof(measured_state), "ERROR"); /* 详细错误仍由当次测量报告给出。 */
    }
    else if (has_measurement && last_measurement.state >= 0) /* 已得到唯一匹配的DIV编号。 */
    {
        snprintf(measured_state, sizeof(measured_state), "DIV%d", last_measurement.state); /* 转为易读状态名。 */
    }
    else if (has_measurement) /* ADC可读但未匹配任何唯一工况。 */
    {
        snprintf(measured_state, sizeof(measured_state), "UNKNOWN"); /* 不把未知结果误写成正常。 */
    }
#if OLED_DISPLAY_ENABLE /* 显示启用时报告参考驱动的初始化状态。 */
    written = snprintf(line, sizeof(line), /* 报告测量、控制脚及参考驱动初始化状态。 */
        "HEARTBEAT_MS=%lu STATE=%s RUN_GPIO=%s TEST_GPIO=%s OLED_INIT=%s ACK=UNCHECKED SCL=%s SDA=%s\r\n", /* 参考驱动忽略 ACK，因此明确写出未检查。 */
        (unsigned long)now_ms, measured_state, /* 显示运行时间和最近一次真实测量的工况。 */
        HAL_GPIO_ReadPin(GPIOB, RUN_POWER_PIN) == GPIO_PIN_SET ? "HIGH" : "LOW", /* 回读 PB13 控制电平。 */
        HAL_GPIO_ReadPin(GPIOB, TEST_POWER_PIN) == GPIO_PIN_SET ? "HIGH" : "LOW", /* 回读 PB12 控制电平。 */
        Oled_IsInitialized() ? "DONE" : "PENDING", /* 仅表示初始化函数是否执行。 */
        HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6) == GPIO_PIN_SET ? "HIGH" : "LOW", /* 读取软件 I2C 时钟线的当前电平。 */
        HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET ? "HIGH" : "LOW"); /* 读取软件 I2C 数据线的当前电平。 */
#else /* 显示已按构建选项关闭：明确写成DISPLAY_OFF，不把关闭误报成OLED通信故障。 */
    written = snprintf(line, sizeof(line), /* 只报告存活、上次工况、两路控制脚和自动采样周期。 */
        "HEARTBEAT_MS=%lu STATE=%s RUN_GPIO=%s TEST_GPIO=%s OLED=DISPLAY_OFF AUTO_MEASURE_MS=%d\r\n", /* 电压数据由每秒的STATE行给出。 */
        (unsigned long)now_ms, measured_state, /* 毫秒计数持续增加即表示主循环没有停住。 */
        HAL_GPIO_ReadPin(GPIOB, RUN_POWER_PIN) == GPIO_PIN_SET ? "HIGH" : "LOW", /* PB13只反映控制脚，非实际电源反馈。 */
        HAL_GPIO_ReadPin(GPIOB, TEST_POWER_PIN) == GPIO_PIN_SET ? "HIGH" : "LOW", /* PB12只反映测试开关控制脚。 */
        AUTO_MEASURE_INTERVAL_MILLISECONDS); /* 打印周期本身，便于核对是否每秒重测。 */
#endif /* 结束显示关闭分支。 */
    if (written < 0 || written >= (int)sizeof(line)) /* 拒绝发送格式化失败或被截断的诊断。 */
    {
        Error_Handler(); /* 发生内部缓冲错误时按原错误路径断电。 */
    }
    if (HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)written, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 只在主循环发送，停止中断仍能立即断电。 */
    {
        Error_Handler(); /* UART真正出错时进入明确的断电停机状态。 */
    }
}

static void ReportOledProgress(int completed_pages) /* 慢速仿真每写完一页就报告实际进度，避免长时间无串口输出。 */
{
    char message[64]; /* 保存简短页进度，不占用显示数据缓冲。 */
    int length = snprintf(message, sizeof(message), "OLED_WRITE_PAGE=%d/8 ACK=UNCHECKED\r\n", completed_pages); /* 每行跨两页；参考驱动不读取应答。 */
    if (length <= 0 || length >= (int)sizeof(message)) /* 拒绝格式化失败或截断的日志。 */
    {
        Error_Handler(); /* 沿用现有异常处理，不报告虚假进度。 */
    }
    if (HAL_UART_Transmit(&huart1, (uint8_t *)message, (uint16_t)length, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 只在两个I2C交易之间发送进度，中断保持开启。 */
    {
        Error_Handler(); /* 串口错误走统一故障处理。 */
    }
}

static void ServiceDisplay(void) /* 只在主循环刷新OLED，中断中不访问I2C和串口。 */
{
#if OLED_DISPLAY_ENABLE /* 显示启用时按测量结果刷新四行文字。 */
    char line[OLED_TEXT_COLUMNS + 1]; /* 参考字库每行最多十六个 ASCII 字符。 */
    char message[80]; /* 保存 OLED 初始化状态的串口信息。 */
    int stopped; /* 保存本次画面开始时的停止状态。 */
    int written; /* 保存串口状态消息长度。 */
    if (Oled_IsInitialized() && display_dirty) /* 仅在初始化完成且画面有变化时刷新。 */
    {
        display_dirty = 0; /* 先清请求，刷新期间的新停止仍能把它重新置位。 */
        stopped = StopIsActive(); /* 停止状态优先于保存的正常结果。 */
        if (stopped) /* 已停止时不显示仍在运行。 */
        {
            Oled_SetLine(0, "DIV STOPPED"); /* 首行优先提示停止状态。 */
        }
        else if (!has_measurement) /* 第一次检测尚未完成。 */
        {
            Oled_SetLine(0, "DIV READY"); /* 测量前不预填结果。 */
        }
        else if (strcmp(last_measurement.error, "OK") != 0) /* 区分ADC或检测错误。 */
        {
            Oled_SetLine(0, "DIV MEASURE ERR"); /* 完整错误仍由串口报告。 */
        }
        else if (last_measurement.state < 0) /* 没有唯一匹配的工况。 */
        {
            Oled_SetLine(0, "DIV UNKNOWN"); /* 未知结果保持运行供电关闭。 */
        }
        else /* 已有正常或故障的唯一判别。 */
        {
            snprintf(line, sizeof(line), "DIV%d %s", last_measurement.state, last_measurement.state == DIV_NORMAL_CASE ? "NORMAL" : "FAULT"); /* 显示真实工况号与判别。 */
            Oled_SetLine(0, line); /* 写入首行。 */
        }
        if (has_measurement && last_measurement.vb_valid) /* 只显示真正取得的VB。 */
        {
            snprintf(line, sizeof(line), "VB:%lu mV", (unsigned long)last_measurement.vb_mv); /* 格式化实际 VB 毫伏值。 */
            Oled_SetLine(1, line); /* 写入第二行。 */
        }
        else /* ADC未读成功时不显示伪造数值。 */
        {
            Oled_SetLine(1, "VB:NA"); /* 未读取时显示 NA。 */
        }
        if (has_measurement && last_measurement.va_valid) /* 独立检查VA有效性。 */
        {
            snprintf(line, sizeof(line), "VA:%lu mV", (unsigned long)last_measurement.va_mv); /* 格式化实际 VA 毫伏值。 */
            Oled_SetLine(2, line); /* 写入第三行。 */
        }
        else /* VA可能因停止或ADC错误而未读取。 */
        {
            Oled_SetLine(2, "VA:NA"); /* 不沿用上轮 VA。 */
        }
        snprintf(line, sizeof(line), "T:%s R:%s", HAL_GPIO_ReadPin(GPIOB, TEST_POWER_PIN) == GPIO_PIN_SET ? "ON" : "OFF", HAL_GPIO_ReadPin(GPIOB, RUN_POWER_PIN) == GPIO_PIN_SET ? "ON" : "OFF"); /* 第四行显示两路控制脚电平。 */
        Oled_SetLine(3, line); /* 控制脚状态是 GPIO 回读，不代表实际电源电压。 */
        Oled_Flush(); /* 按参考工程的四行 8×16 字模显示数据。 */
    }
    if (oled_reported_init_state != Oled_IsInitialized()) /* 首次进入主循环时报告初始化状态。 */
    {
        oled_reported_init_state = Oled_IsInitialized(); /* 保存已报告的初始化状态。 */
        written = snprintf(message, sizeof(message), "OLED_INIT=%s ADDRESS_CONFIGURED_7BIT=0x%02X ACK=UNCHECKED\r\n", oled_reported_init_state ? "DONE" : "PENDING", (unsigned int)Oled_Address()); /* 不把未检查 ACK 的参考驱动误报为在线。 */
        if (written > 0 && written < (int)sizeof(message)) /* 只发送完整消息。 */
        {
            if (HAL_UART_Transmit(&huart1, (uint8_t *)message, (uint16_t)written, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 9600波特下留足发送时间。 */
            {
                Error_Handler(); /* 串口异常时按统一错误路径关闭电源。 */
            }
        }
    }
#else /* 显示关闭时不做任何I2C动作：清掉刷新请求，保证主循环只服务采样与串口。 */
    if (display_dirty) /* 仍然读取标志，避免关闭显示后留下只写不读的变量。 */
    {
        display_dirty = 0; /* 显示不参与验收时立即丢弃刷新请求。 */
    }
#endif /* 结束显示开关分支。 */
}

#endif /* OLED 显示服务和状态报告结束。 */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  power_gpio_ready = 1; /* GPIO 已初始化，从现在起允许停止和电源切换函数真正拉低 PB12、PB13。 */
  AllPowerOff(); /* 自动供电验收：初始化后再次保证 PB12/PB13 均为低电平。 */
#if DIV_REFERENCE_SERIAL_MODE /* 上电标记用于确认已加载本次按截图输出的固件。 */
  const char startup[] = "FW=OLED_STOP_R2 OLED_MODE=TEXT4X16 ACK=UNCHECKED MODE=TWO_NODES VREF_MV=3300 ADC_BITS=12 BAUD=9600 RUN_POWER=NOT_IMPLEMENTED\r\n"; /* 标记本次固件；参考串口分支仍不切换 3 V。 */
  const char calibration_error[] = "STATE=UNKNOWN ERROR=ADC_CALIBRATION_FAILED RUN_POWER=NOT_IMPLEMENTED\r\n"; /* 校准失败时不伪造测量结果或供电状态。 */
#else /* 保留原受控电源模式的上电说明。 */
  const char startup[] = "FW=OLED_STOP_R2 OLED_MODE=TEXT4X16 ACK=UNCHECKED MODE=ON_DEMAND VREF_MV=3300 ADC_BITS=12 BAUD=9600\r\n"; /* 标记本次 OLED 与断电控制修复，便于确认 Proteus 已加载新固件。 */
  const char calibration_error[] = "STATE=UNKNOWN ERROR=ADC_CALIBRATION_FAILED RUN_POWER=OFF TEST_POWER=OFF\r\n"; /* 校准失败时不打开任何被测电路电源。 */
#endif /* 结束启动和校准错误消息的模式选择。 */
  HAL_Delay(TERMINAL_STARTUP_WAIT_MILLISECONDS); /* 等待虚拟终端就绪期间两路输出保持关闭，停止中断继续有效。 */
  if (HAL_UART_Transmit(&huart1, (uint8_t *)startup, sizeof(startup) - 1, UART_TIMEOUT_MILLISECONDS) != HAL_OK) /* 上电先验证终端链路。 */
  {
    Error_Handler(); /* 启动信息发送失败时停止。 */
  }
  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) /* 所有外设初始化之后只校准一次。 */
  {
    HAL_UART_Transmit(&huart1, (uint8_t *)calibration_error, sizeof(calibration_error) - 1, UART_TIMEOUT_MILLISECONDS); /* 尽力报告校准故障。 */
    Error_Handler(); /* 校准失败不得继续采样判别。 */
  }
  retest_button.raw_level = HAL_GPIO_ReadPin(GPIOA, RETEST_BUTTON_PIN); /* 上电时按住复测也不制造额外的按下沿。 */
  retest_button.stable_level = retest_button.raw_level; /* 将当前电平作为初始稳定状态。 */
  retest_button.changed_ms = HAL_GetTick(); /* 从初始化结束开始按钮消抖计时。 */
  buttons_ready = 1; /* 从此允许SysTick记录按键短按。 */
#if OLED_DISPLAY_ENABLE /* 串口已经就绪，从本次文字刷新起报告写入进度。 */
  Oled_SetProgressCallback(ReportOledProgress); /* 使用已有回调报告四行刷新进度，便于定位画面停在哪一行。 */
  SendRetestTrace("OLED_INIT_BEGIN"); /* 初始化之前报告输入状态，区分屏幕初始化与按键检测故障。 */
  Oled_Init(); /* 调用工程已有驱动完成初始化，使主循环能够刷新测量结果和停止状态。 */
  SendRetestTrace("OLED_INIT_END"); /* 表示初始化函数已经返回，不代表驱动检查过 OLED 应答。 */
#endif /* 结束OLED回调注册。 */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) /* 持续执行循环；错误处理中的空循环用于停机。 */
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    ServiceControls(); /* 上电检测一次，随后响应停止和复测，不在运行时自动重测。 */
#if OLED_DISPLAY_ENABLE /* 启用四行 OLED，并保留显示进度与运行状态日志。 */
    ServiceDisplay(); /* 检测结束或停止后刷新已保存的结果。 */
    ServiceStatus(); /* 每秒报告一次程序存活及I2C故障线索，不重复进行ADC检测。 */
#endif /* 结束 OLED 与显示状态服务。 */
    HAL_Delay(CONTROL_POLL_MILLISECONDS); /* 每隔1ms处理一次控制，测量间隔由ServiceControls判断。 */
  }

  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */



/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  DivEmergencyStop(); /* 在关闭中断和停机之前，先关闭被测电路的两路电源。 */
  __disable_irq(); /* 错误时关闭中断，保留失败现场供检查。 */
  while (1) /* 持续执行循环；错误处理中的空循环用于停机。 */
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
