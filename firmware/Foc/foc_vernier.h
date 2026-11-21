#ifndef FOC_VERNIER_H
#define FOC_VERNIER_H

#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOC_VERNIER_MOTOR_TURNS   ((int32_t)CFG_VERNIER_TEETH2)

/**
 * @brief  设置第二编码器的零偏。
 *
 * 第二编码器的磁铁装配角度每台不同，会让圈号余数整体偏离整数。零偏由校准的
 * 正反转测得，解码前先减掉。
 *
 * @param[in] offset_index  零偏，单位是圈号，范围 ±0.5。
 */
void Foc_VernierSetOffset(float offset_index);

/**
 * @brief  读取当前使用的第二编码器零偏。
 * @return 零偏，单位是圈号。
 */
float Foc_VernierGetOffset(void);

/**
 * @brief  计算未扣除零偏的圈号实数值，供校准测量零偏使用。
 * @param[in] raw1  电机端单圈计数。
 * @param[in] raw2  第二编码器单圈计数。
 * @return 圈号，范围 [0, N2)，理想情况下是整数。
 */
float Foc_VernierIndexRaw(uint16_t raw1, uint16_t raw2);

/**
 * @brief  Recover the motor-shaft absolute angle from a 30/31 vernier pair.
 * @param[in]  raw1         Motor-end single-turn code.
 * @param[in]  raw2         Aux-gear single-turn code.
 * @param[out] raw_abs_out  Motor absolute angle in the sensor frame, [0, 2π N2).
 * @return 1 if the residue is consistent with an integer turn.
 */
uint8_t Foc_VernierRawAbs(uint16_t raw1, uint16_t raw2, float *raw_abs_out);

/**
 * @brief  Map a sensor-frame absolute angle into the FOC encoder-dir frame.
 * @param[in] raw_abs      Sensor-frame motor absolute angle.
 * @param[in] encoder_dir  Motor encoder direction from calibration (+1 / -1).
 * @return Aligned multi-turn mechanical angle.
 */
float Foc_VernierAlignAbs(float raw_abs, int8_t encoder_dir);

#ifdef __cplusplus
}
#endif

#endif /* FOC_VERNIER_H */
