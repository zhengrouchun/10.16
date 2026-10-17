/* 解决ADC采样与判别流程集中在main.c：测量模块实现。 */
#include "main.h"
#include "adc.h"
#include "usart.h"
#include "div_config.h"
#include "div_app_types.h"
#include "div_diagnosis.h"
#include "div_power.h"
#include "div_display.h"
#include "div_measurement.h"
#include <stdio.h>
#include <string.h>
static const char *ReadAdcChannel(uint32_t channel, uint32_t *raw, uint32_t *conversions) /* 失败返回阶段名，成功返回 OK。 */
{
    ADC_ChannelConfTypeDef selected = {0}; /* 全部配置字段清零，随后明确设置所需字段。 */
    if (DivPower_StopIsActive()) /* 停止若发生在上一通道转换期间，不再启动下一通道。 */
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
    if (DivPower_StopIsActive()) /* 配置完成到启动之前再次检查停止。 */
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
    DivDisplay_SetMeasurement(result); /* 将完整结果交给显示模块缓存。 */
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

void DivMeasurement_Run(void) /* 每轮重新采集VB、VA，再换算、判别并输出一条报告。 */
{
    DivMeasurement result = {0}; /* 清空这一轮记录；两个有效标志均为假。 */
    uint32_t started_ms = HAL_GetTick(); /* 先记录准备开始时刻；截图模式在电源稳定后重置采样计时。 */
    result.state = DIV_UNKNOWN; /* 在读完两点并成功判别之前维持未知。 */
    result.error = "STOP_REQUESTED"; /* 建立测试状态失败时报告停止，不能伪造 ADC 数据。 */
    if (!DivPower_Select(TEST_POWER_PIN) || !DivPower_WaitWithoutStop(TEST_POWER_OBSERVATION_MILLISECONDS)) /* 关闭运行、互锁等待、打开测试并保持1.5秒供观察。 */
    {
        DivPower_AllOff(); /* 建立测试状态失败时保证双路关闭。 */
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
    if (DivPower_StopIsActive()) /* 即使停止发生在 ADC 转换期间，也禁止接受结果并开启运行。 */
    {
        result.state = DIV_UNKNOWN; /* 已取消的检测不作为正常或故障验收结果。 */
        result.error = "STOP_REQUESTED"; /* 报告本轮取消原因。 */
        DivPower_AllOff(); /* 保持停止时的双路关闭。 */
    }
#if !DIV_REFERENCE_SERIAL_MODE /* 截图模式不自动切到3V，避免下一轮采到运行态电压。 */
    else if (strcmp(result.error, "OK") == 0 && result.vb_valid && result.va_valid && result.state == DIV_NORMAL_CASE) /* 两点均成功且唯一匹配正常才进入运行。 */
    {
        if (!DivPower_Select(RUN_POWER_PIN) || DivPower_StopIsActive()) /* 先断开测试电源，再等待 5 ms，最后尝试开启运行。 */
        {
            result.state = DIV_UNKNOWN; /* 切换被停止取消时拒绝正常结果。 */
            result.error = "STOP_REQUESTED"; /* 输出明确的取消原因。 */
            DivPower_AllOff(); /* 保证双路最终关闭。 */
        }
    }
    else /* 已知故障、未知工况或任何 ADC 失败都不允许进入运行。 */
    {
        DivPower_AllOff(); /* 检测完成后连测试电源也关闭，保持等待复测状态。 */
    }
#else /* 连续串口测试模式完成本轮后关闭测试使能，下一轮重新建立1V条件。 */
    DivPower_AllOff(); /* 将PB12和PB13拉低；图中手动SW1/SW7仍需用户实际操作。 */
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

