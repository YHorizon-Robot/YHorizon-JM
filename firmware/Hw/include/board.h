#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Board_GetUid64(uint8_t uid[8]);
void Board_Init(void);
void Board_DwtInit(void);
uint32_t Board_DwtGetCycles(void);
uint32_t Board_DwtCyclesToNs(uint32_t cycles);
void Board_LedSet(uint8_t on);
void Board_LedToggle(void);
uint8_t Board_ButtonRaw(void);
void Error_Handler(void);

/**
 * @brief  毫秒时基。由 SysTick 调用 @ref Board_IncTick 推进。
 */
uint32_t Board_GetTickMs(void);
void Board_DelayMs(uint32_t ms);
void Board_IncTick(void);

/**
 * @brief  保存 PRIMASK 并关中断。
 * @return 关中断前的 PRIMASK，交给 @ref Board_IrqUnlock。
 */
uint32_t Board_IrqLock(void);

/**
 * @brief  恢复 @ref Board_IrqLock 返回的 PRIMASK。
 */
void Board_IrqUnlock(uint32_t primask);

/**
 * @brief  软件复位。
 */
void Board_SystemReset(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
