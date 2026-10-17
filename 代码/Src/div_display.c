/* 解决OLED刷新和运行状态报告混在 main.c：显示模块实现。 */
#include "main.h"
#include "usart.h"
#include "div_config.h"
#include "div_app_types.h"
#include "div_power.h"
#include "div_display.h"
#include "oled_display.h"
#include "OLED.h"
#include <stdio.h>
#include <string.h>

/* 显示模块私有缓存：只保存最近一轮真实采样结果。 */
static volatile int display_dirty = 1; /* 请求主循环刷新屏幕。 */
static DivMeasurement last_measurement; /* 最近一次检测结果。 */
static int has_measurement = 0; /* 标识是否已经得到第一轮检测。 */
static int oled_reported_init_state = -1; /* 上次报告的OLED初始化状态。 */
#if OLED_DISPLAY_ENABLE /* OLED启用时才记录定时状态报告时间。 */
static uint32_t last_status_ms = 0; /* 上次心跳输出时间。 */
#endif /* 结束OLED专用计时变量。 */

void DivDisplay_RequestRefresh(void) /* 请求主循环重新绘制当前状态。 */
{
    display_dirty = 1; /* 中断中只修改刷新标志，不访问OLED总线。 */
}

void DivDisplay_SetMeasurement(const DivMeasurement *measurement) /* 接收检测结果并安排一次刷新。 */
{
    last_measurement = *measurement; /* 复制结果，避免依赖测量模块的局部变量。 */
    has_measurement = 1; /* 记录至少完成过一次检测。 */
    display_dirty = 1; /* 通知主循环刷新显示。 */
}

void DivDisplay_Initialize(void) /* 初始化已有OLED驱动并注册进度回调。 */
{
    Oled_SetProgressCallback(DivDisplay_ReportProgress); /* 在每次屏幕写入后报告实际页进度。 */
    Oled_Init(); /* 调用现有驱动初始化OLED。 */
}

#if OLED_DISPLAY_ENABLE /* 显示启用时编译状态报告和屏幕刷新流程。 */
void DivDisplay_ServiceStatus(void) /* 每秒发送一次存活与总线状态，持续运行不等于重复进行DIV检测。 */
{
    uint32_t now_ms = HAL_GetTick(); /* 获取当前毫秒时刻，证明主循环仍在运行。 */
    static char line[REPORT_BUFFER_SIZE]; /* 主循环独占的心跳缓冲区使用静态存储，为中断保留栈空间。 */
    char measured_state[20] = "WAITING"; /* 首轮测量前标明尚无判别结果。 */
    int written; /* 保存格式化的字符数量并检查是否溢出。 */
    if (now_ms - last_status_ms < STATUS_INTERVAL_MILLISECONDS) /* 每秒最多发送一条，避免串口占用过久。 */
    {
        return; /* 时间未到时继续及时处理按钮和停止。 */
    }
    last_status_ms = now_ms; /* 记录本次报告时刻，毫秒回绕后差值仍有效。 */
    if (DivPower_StopIsActive()) /* 停止状态比上一次保存的正常测量结果优先。 */
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
        "HEARTBEAT_MS=%lu STATE=%s RUN_GPIO=%s TEST_GPIO=%s OLED_INIT=%s ACK=UNCHECKED SCL=%s SDA=%s RESULT=LAST_TEST\r\n", /* 明确心跳引用上次测量，不代表刚重新采样；参考驱动仍未检查 ACK。 */
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

void DivDisplay_ReportProgress(int completed_pages) /* 慢速仿真每写完一页就报告实际进度，避免长时间无串口输出。 */
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

void DivDisplay_Service(void) /* 只在主循环刷新OLED，中断中不访问I2C和串口。 */
{
#if OLED_DISPLAY_ENABLE /* 显示启用时按测量结果刷新四行文字。 */
    char line[OLED_TEXT_COLUMNS + 1]; /* 参考字库每行最多十六个 ASCII 字符。 */
    char message[80]; /* 保存 OLED 初始化状态的串口信息。 */
    int stopped; /* 保存本次画面开始时的停止状态。 */
    int written; /* 保存串口状态消息长度。 */
    if (Oled_IsInitialized() && display_dirty) /* 仅在初始化完成且画面有变化时刷新。 */
    {
        display_dirty = 0; /* 先清请求，刷新期间的新停止仍能把它重新置位。 */
        stopped = DivPower_StopIsActive(); /* 停止状态优先于保存的正常结果。 */
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
