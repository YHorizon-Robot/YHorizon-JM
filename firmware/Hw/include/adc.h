#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Regular group samples Vbus in software. The injected group samples
 *         both phase currents in the low-side conduction window at the PWM peak.
 */
void Adc_Init(void);
void Adc_Service(void);
/** Force a new regular conversion in main context, e.g. after Flash writes. */
void Adc_RefreshVbus(void);
/** @return 最近一次母线电压。转换失败或超过 5 个采样周期没更新时返回 0。 */
float Adc_GetVbusVolts(void);
uint16_t Adc_GetVbusRaw(void);

/**
 * @brief  上电探测到 PA3 上焊了 NTC 时返回 1。没焊、上拉到满量程或悬空都返回 0。
 */
uint8_t Adc_TempSensorPresent(void);

/**
 * @brief  最近一次 NTC 读数，约 100 ms 更新一次。
 * @param[out] raw_out  ADC 原始值。
 * @return 1 表示 NTC 在位且最近 500 ms 内有新读数；读数是否在范围内由调用方判断。
 */
uint8_t Adc_GetTempRaw(uint16_t *raw_out);

/**
 * @brief  上电、PWM 尚未打开时采到的相电流零点，单位 ADC LSB。
 */
void Adc_GetCurrentOffset(uint16_t *ia_offset, uint16_t *ib_offset);

/** @return 1 only when every startup offset sample passed validation. */
uint8_t Adc_CurrentSenseReady(void);

/**
 * @brief  Consume one finished injected pair.
 * @return 1: a fresh valid pair; 0: no completed pair; 2: invalid calibration
 *         or rail-clipped sample. Output arguments are written only on success.
 */
uint8_t Adc_ReadPhaseCurrents(float *ia_a, float *ib_a);

#ifdef __cplusplus
}
#endif

#endif /* ADC_H */
