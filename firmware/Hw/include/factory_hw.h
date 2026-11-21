#ifndef FACTORY_HW_H
#define FACTORY_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  晶振丢失后按内部振荡器重配毫秒时基。
 */
void FactoryHw_UseInternalClock(void);

/**
 * @brief  CAN 控制器静默环回。
 * @return 0 成功，1 初始化失败，2 无邮箱，3 超时，4 数据不符。
 */
uint8_t FactoryHw_CanLoopback(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_HW_H */
