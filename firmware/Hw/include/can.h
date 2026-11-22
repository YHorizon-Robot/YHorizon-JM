#ifndef CAN_H
#define CAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t id;
#if CFG_CAN_COMPAT
  uint32_t received_ms;
  uint8_t extended, dlc;
#endif
  uint8_t  data[8];
} CanFrame_t;

void Can_Init(uint8_t node_id);
void Can_ProcessRxIrq(void);
void Can_OnBusOffIrq(void);
void Can_Service(void);
void Can_StopForFlash(void);
void Can_Restart(void);
uint8_t Can_PopRx(CanFrame_t *frame);
uint8_t Can_Send(uint32_t id, const uint8_t data[8]);
#if CFG_CAN_COMPAT
uint8_t Can_SendFrame(const CanFrame_t *frame);
uint8_t Can_RxLost(void);
void Can_GetLossCounts(uint32_t *rx, uint32_t *tx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CAN_H */
