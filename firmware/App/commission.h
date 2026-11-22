#ifndef COMMISSION_H
#define COMMISSION_H

#include "cali_nv.h"

void Commission_Init(void);
void Commission_Service(void);
uint8_t Commission_Active(void);
uint8_t Commission_BlocksDrive(void);
void Commission_Abort(void);
/* Called at the PWM valley, after the encoder update and before the next duty is written. */
uint8_t Commission_Pwm(uint16_t raw, float motor_rad, float motor_speed);
uint8_t Commission_Current(float elec, float ia, float ib, float iq_ref);
float Commission_LimitIq(float target, int8_t direction);
uint8_t Commission_DAxisStep(void);
uint8_t Commission_Handle(const uint8_t data[8], uint32_t node, uint8_t calibrated);
uint8_t Commission_MergeAlignment(CaliNvData_t *nv, uint8_t legacy_valid);
void Commission_ApplyControl(void);
float Commission_LegacyZero(float aligned_zero);
uint8_t Commission_OwnsGains(void);
uint8_t Commission_SaveGains(void);
float Commission_PhaseResistance(void);
float Commission_CurrentBandwidth(void);
float Commission_Cogging(uint16_t raw);

#endif
