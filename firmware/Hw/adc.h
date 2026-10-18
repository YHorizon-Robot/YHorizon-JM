#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  ADC0 regular = PB0/IN8 Vbus (software).
 *         Inserted = PA0/IN0 CURA + PA1/IN1 CURB, TIMER0 CH3 near the carrier peak.
 */
void Adc_Init(void);
void Adc_Service(void);
float Adc_GetVbusVolts(void);
uint16_t Adc_GetVbusRaw(void);

/**
 * @brief  Consume one finished injected pair.
 * @return 1 if a new sample was available.
 */
uint8_t Adc_ReadPhaseCurrents(float *ia_a, float *ib_a);

#ifdef __cplusplus
}
#endif

#endif /* ADC_H */
