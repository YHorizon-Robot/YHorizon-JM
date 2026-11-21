#ifndef MULTI_AXIS_H
#define MULTI_AXIS_H
#include <stdint.h>

uint8_t MultiAxis_Busy(void);
void MultiAxis_Stop(void);
void MultiAxis_Command(const uint8_t data[8], uint8_t node, uint8_t calibrated);
void MultiAxis_OnSyncIrq(const uint8_t data[8]);
uint8_t MultiAxis_Control(float position, float velocity, float *reference, float *feedforward);
void MultiAxis_Service(void);
#endif
