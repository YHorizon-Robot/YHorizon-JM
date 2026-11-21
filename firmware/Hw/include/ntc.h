#ifndef NTC_H
#define NTC_H

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file  ntc.h
 * @brief 板载 10 kΩ NTC（SDNT1608X103F3950FTF，B=3950）的换算与焊接检测。
 *
 * 接法：3.3 V — 10 kΩ — PA3 — NTC — GND，ADC 参考也是 3.3 V，读数与电源无关。
 *
 * 没焊 NTC 时 PA3 要么被 10 kΩ 拉到满量程，要么整个分压都没焊、引脚悬空。
 * 满量程用读数范围就能排除；悬空引脚的读数取决于采样电容上残留的电荷，
 * 可能落在正常范围里，必须在上电时主动探测，见 Ntc_ProbeOk()。
 */

#define NTC_FULLSCALE       (4095U)
#define NTC_R_PULLUP_OHM    (10000.0f)
#define NTC_R25_OHM         (10000.0f)
#define NTC_BETA            (3950.0f)
#define NTC_T25_K           (298.15f)

/** @brief 读数下限，约 160 °C。再低按短路或下拉缺失处理。 */
#define NTC_RAW_MIN         (60U)
/** @brief 读数上限，约 -35 °C。再高按 NTC 未焊、被上拉到满量程处理。 */
#define NTC_RAW_MAX         (3950U)
/** @brief 上电探测时先拉低、再拉高两次读数允许的最大差值。 */
#define NTC_PROBE_MAX_DIFF  (200U)

/**
 * @brief  读数是否落在 NTC 能给出的范围内。
 */
static inline uint8_t Ntc_RawInRange(uint16_t raw)
{
  return ((raw >= NTC_RAW_MIN) && (raw <= NTC_RAW_MAX)) ? 1U : 0U;
}

/**
 * @brief  上电探测的判定。
 *
 * 探测时 PA3 先推挽输出低电平再切回模拟输入，等分压稳定后读一次 raw_low；
 * 再输出高电平、切回、等待、读一次 raw_high。焊了 NTC 时引脚由分压驱动，
 * 两次读数相同；悬空时引脚只靠寄生电容保持刚才驱动的电平，两次相差接近满量程，
 * 或者被漏电拉到轨上而超出范围。
 *
 * @param[in] raw_low   拉低后的读数。
 * @param[in] raw_high  拉高后的读数。
 * @return 1 表示 NTC 在位。
 */
static inline uint8_t Ntc_ProbeOk(uint16_t raw_low, uint16_t raw_high)
{
  const uint16_t diff = (raw_high > raw_low) ? (uint16_t)(raw_high - raw_low)
                                             : (uint16_t)(raw_low - raw_high);
  return ((Ntc_RawInRange(raw_low) != 0U) && (Ntc_RawInRange(raw_high) != 0U) &&
          (diff <= NTC_PROBE_MAX_DIFF)) ? 1U : 0U;
}

/**
 * @brief  ADC 读数换算成摄氏度，B 值公式。调用前应确认 Ntc_RawInRange()。
 */
static inline float Ntc_RawToCelsius(uint16_t raw)
{
  const float r = NTC_R_PULLUP_OHM * (float)raw / (float)(NTC_FULLSCALE - raw);
  const float inv_t = (1.0f / NTC_T25_K) + (logf(r / NTC_R25_OHM) / NTC_BETA);
  return (1.0f / inv_t) - 273.15f;
}

#ifdef __cplusplus
}
#endif

#endif /* NTC_H */
