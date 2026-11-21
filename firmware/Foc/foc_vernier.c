#include "foc_vernier.h"

#include "encoder.h"
#include "foc_math.h"

#include <math.h>

/** @brief 校准得到的第二编码器零偏，单位是圈号（1 对应第二编码器 1/N2 圈）。 */
static float s_offset_index;

static float Foc_VernierWrap01(float x)
{
  x -= floorf(x);
  if (x < 0.0f)
  {
    x += 1.0f;
  }
  else if (x >= 1.0f)
  {
    x -= 1.0f;
  }
  return x;
}

void Foc_VernierSetOffset(float offset_index)
{
  s_offset_index = isfinite(offset_index) ? Foc_Clamp(offset_index, -0.5f, 0.5f) : 0.0f;
}

float Foc_VernierGetOffset(void)
{
  return s_offset_index;
}

float Foc_VernierIndexRaw(uint16_t raw1, uint16_t raw2)
{
  const float inv_cpr = 1.0f / (float)ENCODER_CPR;
  const float n1 = (float)CFG_VERNIER_TEETH1;
  const float n2 = (float)CFG_VERNIER_TEETH2;
  const float u = (float)raw1 * inv_cpr;
  float v = (float)CFG_ENC2_DIR * ((float)raw2 * inv_cpr);

  v = Foc_VernierWrap01(v - (CFG_ENC2_OFFSET_RAD * (1.0f / FOC_TWO_PI)));
  /* Meshed gears: v + (N1/N2)*u ≡ n/N2 (mod 1), n = 0..N2-1. */
  return Foc_VernierWrap01(v + ((n1 / n2) * u)) * n2;
}

uint8_t Foc_VernierRawAbs(uint16_t raw1, uint16_t raw2, float *raw_abs_out)
{
  const float n2 = (float)CFG_VERNIER_TEETH2;
  float n_f;
  int32_t n;

  if (raw_abs_out == 0)
  {
    return 0U;
  }

  n_f = Foc_VernierIndexRaw(raw1, raw2) - s_offset_index;
  if (n_f < 0.0f)
  {
    n_f += n2;
  }
  else if (n_f >= n2)
  {
    n_f -= n2;
  }
  /** n_f 在 [0, N2) 内，取整后可能等于 N2，对应圈号 0。 */
  n = (int32_t)(n_f + 0.5f);
  if (fabsf(n_f - (float)n) > CFG_VERNIER_RESIDUE_TOL)
  {
    return 0U;
  }
  if (n >= (int32_t)CFG_VERNIER_TEETH2)
  {
    n = 0;
  }

  *raw_abs_out = (((float)n + ((float)raw1 / (float)ENCODER_CPR)) * FOC_TWO_PI);
  return 1U;
}

float Foc_VernierAlignAbs(float raw_abs, int8_t encoder_dir)
{
  if (encoder_dir >= 0)
  {
    return raw_abs;
  }
  return -raw_abs;
}
