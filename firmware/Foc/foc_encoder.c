#include "foc_encoder.h"

#include <math.h>

#define FOC_VEL_LPF_SQRT2  (1.41421356237f)

static void Foc_EncoderInitVelLpf(Foc_Encoder_t *encoder, float sample_hz, float fc_hz)
{
  const float T = 1.0f / sample_hz;
  const float w = FOC_TWO_PI * fc_hz;
  const float K = 2.0f / T;
  const float w2 = w * w;
  const float a0 = (K * K) + (FOC_VEL_LPF_SQRT2 * K * w) + w2;

  encoder->lpf_b0 = w2 / a0;
  encoder->lpf_b1 = (2.0f * w2) / a0;
  encoder->lpf_b2 = encoder->lpf_b0;
  encoder->lpf_a1 = ((-2.0f * K * K) + (2.0f * w2)) / a0;
  encoder->lpf_a2 = ((K * K) - (FOC_VEL_LPF_SQRT2 * K * w) + w2) / a0;
}

static void Foc_EncoderClearVelLpf(Foc_Encoder_t *encoder)
{
  encoder->lpf_x1 = 0.0f;
  encoder->lpf_x2 = 0.0f;
  encoder->lpf_y1 = 0.0f;
  encoder->lpf_y2 = 0.0f;
}

static float Foc_EncoderLpfStep(Foc_Encoder_t *encoder, float x)
{
  const float y = (encoder->lpf_b0 * x)
                + (encoder->lpf_b1 * encoder->lpf_x1)
                + (encoder->lpf_b2 * encoder->lpf_x2)
                - (encoder->lpf_a1 * encoder->lpf_y1)
                - (encoder->lpf_a2 * encoder->lpf_y2);

  encoder->lpf_x2 = encoder->lpf_x1;
  encoder->lpf_x1 = x;
  encoder->lpf_y2 = encoder->lpf_y1;
  encoder->lpf_y1 = y;
  return y;
}

static float Foc_EncoderMechFromElecVel(const Foc_Encoder_t *encoder, float omega_e)
{
  if (encoder->pole_pairs == 0U)
  {
    return 0.0f;
  }
  return omega_e / (float)encoder->pole_pairs;
}

/**
 * @brief 由整圈数和单圈角度合成多圈角度。
 *
 * 多圈角度到几十弧度以后，float 的最小刻度和一个编码器计数相当，逐周期累加增量
 * 会让每一步都按同一方向舍入，位置比例误差可到 0.5%。整圈数单独用整数记，
 * 多圈角度每次重新合成，误差不累积。
 */
static void Foc_EncoderCompose(Foc_Encoder_t *encoder)
{
  encoder->mech_unwrapped = ((float)encoder->mech_turns * FOC_TWO_PI) + encoder->mech_wrapped;
}

/**
 * @brief 把给定的多圈角度拆成整圈数，单圈部分以当前 mech_wrapped 为准。
 */
static void Foc_EncoderSplit(Foc_Encoder_t *encoder, float unwrapped_rad)
{
  encoder->mech_turns = (int32_t)floorf(
      ((unwrapped_rad - encoder->mech_wrapped) / FOC_TWO_PI) + 0.5f);
  Foc_EncoderCompose(encoder);
}

void Foc_EncoderInit(Foc_Encoder_t *encoder, const Foc_EncoderConfig_t *config)
{
  encoder->pole_pairs = config->pole_pairs;
  encoder->direction = (config->direction < 0) ? -1 : 1;
  encoder->electrical_offset_rad = config->electrical_offset_rad;
  encoder->initialized = 0U;
  encoder->last_raw_mech = 0.0f;
  encoder->mech_wrapped = 0.0f;
  encoder->mech_turns = 0;
  encoder->mech_unwrapped = 0.0f;
  encoder->mech_zero = 0.0f;
  encoder->last_elec = 0.0f;
  encoder->vel_hat = 0.0f;
  encoder->elec_angle = 0.0f;
  Foc_EncoderInitVelLpf(encoder, config->sample_hz, config->vel_lpf_hz);
  Foc_EncoderClearVelLpf(encoder);
}

void Foc_EncoderSetAlignment(Foc_Encoder_t *encoder, int8_t direction, float electrical_offset_rad)
{
  encoder->direction = (direction < 0) ? -1 : 1;
  encoder->electrical_offset_rad = electrical_offset_rad;
}

void Foc_EncoderSetPolePairs(Foc_Encoder_t *encoder, uint8_t pole_pairs)
{
  encoder->pole_pairs = pole_pairs;
}

void Foc_EncoderReset(Foc_Encoder_t *encoder, float raw_mech_angle_rad)
{
  const float aligned = Foc_ApplyEncoderDirToMechTheta(raw_mech_angle_rad, encoder->direction);
  encoder->last_raw_mech = raw_mech_angle_rad;
  encoder->mech_wrapped = aligned;
  encoder->mech_turns = 0;
  encoder->mech_unwrapped = aligned;
  encoder->elec_angle = Foc_WrapAngle0To2Pi(
      ((float)encoder->pole_pairs * aligned) + encoder->electrical_offset_rad);
  encoder->last_elec = encoder->elec_angle;
  encoder->vel_hat = 0.0f;
  Foc_EncoderClearVelLpf(encoder);
  encoder->initialized = 1U;
}

void Foc_EncoderSeedAbs(Foc_Encoder_t *encoder, float raw_mech_angle_rad, float aligned_abs_rad)
{
  encoder->last_raw_mech = raw_mech_angle_rad;
  encoder->mech_wrapped = Foc_ApplyEncoderDirToMechTheta(raw_mech_angle_rad, encoder->direction);
  Foc_EncoderSplit(encoder, aligned_abs_rad);
  encoder->elec_angle = Foc_WrapAngle0To2Pi(
      ((float)encoder->pole_pairs * encoder->mech_wrapped) + encoder->electrical_offset_rad);
  encoder->last_elec = encoder->elec_angle;
  encoder->initialized = 1U;
}

void Foc_EncoderSeedSingleTurn(Foc_Encoder_t *encoder, float raw_mech_angle_rad, float zero_aligned_rad)
{
  Foc_EncoderReset(encoder, raw_mech_angle_rad);
  /* A single-turn sensor cannot recover the old turn count. Choose the
   * equivalent directed angle nearest the persistent zero, including when
   * SET_ZERO was issued after several turns or across the sensor wrap. */
  Foc_EncoderSplit(encoder, zero_aligned_rad
                            + Foc_WrapAngleToPi(encoder->mech_wrapped - zero_aligned_rad));
  encoder->mech_zero = zero_aligned_rad;
}

void Foc_EncoderShiftTurns(Foc_Encoder_t *encoder, int32_t turns)
{
  encoder->mech_turns += turns;
  Foc_EncoderCompose(encoder);
}

void Foc_EncoderSetZero(Foc_Encoder_t *encoder)
{
  encoder->mech_zero = encoder->mech_unwrapped;
}

void Foc_EncoderSetZeroAbs(Foc_Encoder_t *encoder, float zero_aligned_rad)
{
  encoder->mech_zero = zero_aligned_rad;
}

void Foc_EncoderPredict(Foc_Encoder_t *encoder, float dt_s)
{
  float d_mech;
  float d_elec;
  float omega_e;

  if ((encoder->initialized == 0U) || (dt_s <= 0.0f))
  {
    return;
  }

  d_mech = encoder->vel_hat * dt_s;
  d_elec = d_mech * (float)encoder->pole_pairs;
  {
    const float next = encoder->mech_wrapped + d_mech;
    if (next >= FOC_TWO_PI)
    {
      encoder->mech_turns++;
    }
    else if (next < 0.0f)
    {
      encoder->mech_turns--;
    }
    encoder->mech_wrapped = Foc_WrapAngle0To2Pi(next);
    Foc_EncoderCompose(encoder);
  }
  encoder->elec_angle = Foc_WrapAngle0To2Pi(encoder->elec_angle + d_elec);
  encoder->last_elec = encoder->elec_angle;
  omega_e = encoder->vel_hat * (float)encoder->pole_pairs;
  encoder->vel_hat = Foc_EncoderMechFromElecVel(encoder, Foc_EncoderLpfStep(encoder, omega_e));
}

uint8_t Foc_EncoderUpdate(Foc_Encoder_t *encoder, float raw_mech_angle_rad, float dt_s)
{
  float aligned;
  float delta;
  float omega_e;

  aligned = Foc_ApplyEncoderDirToMechTheta(raw_mech_angle_rad, encoder->direction);

  if (encoder->initialized == 0U)
  {
    Foc_EncoderReset(encoder, raw_mech_angle_rad);
    return 1U;
  }

  if (dt_s <= 0.0f)
  {
    return 0U;
  }

  /** 单圈角度相邻两次相差超过半圈，说明跨过了 0/2π 接缝。 */
  delta = aligned - encoder->mech_wrapped;
  if (delta < -FOC_PI)
  {
    encoder->mech_turns++;
  }
  else if (delta >= FOC_PI)
  {
    encoder->mech_turns--;
  }
  encoder->last_raw_mech = raw_mech_angle_rad;
  encoder->mech_wrapped = aligned;
  Foc_EncoderCompose(encoder);
  encoder->elec_angle = Foc_WrapAngle0To2Pi(
      ((float)encoder->pole_pairs * aligned) + encoder->electrical_offset_rad);

  omega_e = Foc_WrapAngleToPi(encoder->elec_angle - encoder->last_elec) / dt_s;
  encoder->last_elec = encoder->elec_angle;
  encoder->vel_hat = Foc_EncoderMechFromElecVel(encoder, Foc_EncoderLpfStep(encoder, omega_e));

  return 1U;
}

float Foc_EncoderGetMechUnwrapped(const Foc_Encoder_t *encoder)
{
  return encoder->mech_unwrapped;
}

float Foc_EncoderGetPosition(const Foc_Encoder_t *encoder)
{
  return encoder->mech_unwrapped - encoder->mech_zero;
}

float Foc_EncoderGetVelocity(const Foc_Encoder_t *encoder)
{
  return encoder->vel_hat;
}

float Foc_EncoderGetElectrical(const Foc_Encoder_t *encoder)
{
  return encoder->elec_angle;
}

float Foc_EncoderGetMechWrapped(const Foc_Encoder_t *encoder)
{
  return encoder->mech_wrapped;
}

float Foc_EncoderGetLastRaw(const Foc_Encoder_t *encoder)
{
  return encoder->last_raw_mech;
}

uint8_t Foc_EncoderIsReady(const Foc_Encoder_t *encoder)
{
  return encoder->initialized;
}
