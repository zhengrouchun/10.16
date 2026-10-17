#ifndef DIV_CONTROL_H
#define DIV_CONTROL_H

void DivControl_InitializeButtons(void);
void DivControl_OnRetestInterrupt(void);
void DivButtonsTick(void); /* 兼容旧接口；EXTI4 方案为空函数。 */
void DivControl_Trace(const char *event);
void DivControl_RunInitialDetection(void); /* OLED 初始化后直接执行上电首轮检测。 */
void DivControl_Service(void);

#endif /* DIV_CONTROL_H */
