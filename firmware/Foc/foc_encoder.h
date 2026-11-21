#ifndef FOC_ENCODER_H
#define FOC_ENCODER_H

#include <stdint.h>
#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t pole_pairs;
  int8_t  direction;
  float   electrical_offset_rad;
  float   vel_lpf_hz;
  float   sample_hz;
} Foc_EncoderConfig_t;

typedef struct
{
  uint8_t pole_pairs;
  int8_t  direction;
  float   electrical_offset_rad;
  uint8_t initialized;
  float   last_raw_mech;
  float   mech_wrapped;
  /** @brief 整圈数。多圈角度 = mech_turns × 2π + mech_wrapped，不逐周期累加浮点增量。 */
  int32_t mech_turns;
  float   mech_unwrapped;
  float   mech_zero;
  float   last_elec;
  float   vel_hat;
  float   elec_angle;
  float   lpf_b0;
  float   lpf_b1;
  float   lpf_b2;
  float   lpf_a1;
  float   lpf_a2;
  float   lpf_x1;
  float   lpf_x2;
  float   lpf_y1;
  float   lpf_y2;
} Foc_Encoder_t;

void Foc_EncoderInit(Foc_Encoder_t *encoder, const Foc_EncoderConfig_t *config);
void Foc_EncoderReset(Foc_Encoder_t *encoder, float raw_mech_angle_rad);
void Foc_EncoderSeedAbs(Foc_Encoder_t *encoder, float raw_mech_angle_rad, float aligned_abs_rad);
/* Restore a single-turn sensor in the revolution nearest the saved zero.
 * Initial position is in [-pi, pi); runtime updates still accumulate turns. */
void Foc_EncoderSeedSingleTurn(Foc_Encoder_t *encoder, float raw_mech_angle_rad, float zero_aligned_rad);
/**
 * @brief  把多圈角度平移整数圈，单圈角度、电角度和速度都不变。
 * @param[in,out] encoder  编码器状态。
 * @param[in]     turns    平移的电机圈数，可为负。
 */
void Foc_EncoderShiftTurns(Foc_Encoder_t *encoder, int32_t turns);
void Foc_EncoderSetZero(Foc_Encoder_t *encoder);
void Foc_EncoderSetZeroAbs(Foc_Encoder_t *encoder, float zero_aligned_rad);
uint8_t Foc_EncoderUpdate(Foc_Encoder_t *encoder, float raw_mech_angle_rad, float dt_s);
void Foc_EncoderPredict(Foc_Encoder_t *encoder, float dt_s);
void Foc_EncoderSetAlignment(Foc_Encoder_t *encoder, int8_t direction, float electrical_offset_rad);
void Foc_EncoderSetPolePairs(Foc_Encoder_t *encoder, uint8_t pole_pairs);

float Foc_EncoderGetMechUnwrapped(const Foc_Encoder_t *encoder);
float Foc_EncoderGetPosition(const Foc_Encoder_t *encoder);
float Foc_EncoderGetVelocity(const Foc_Encoder_t *encoder);
float Foc_EncoderGetElectrical(const Foc_Encoder_t *encoder);
float Foc_EncoderGetMechWrapped(const Foc_Encoder_t *encoder);
float Foc_EncoderGetLastRaw(const Foc_Encoder_t *encoder);
uint8_t Foc_EncoderIsReady(const Foc_Encoder_t *encoder);

#ifdef __cplusplus
}
#endif

#endif /* FOC_ENCODER_H */
