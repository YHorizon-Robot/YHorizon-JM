#include "foc_current.h"

#include "config.h"

#include <math.h>
#include <string.h>

/* Scale both axes together. Normalizing first avoids overflow for large but
 * finite commands and preserves the requested voltage/current direction. */
static uint8_t Foc_CurrentLimitVector(float limit, float *d, float *q)
{
  const float peak = fmaxf(fabsf(*d), fabsf(*q));
  float dn;
  float qn;
  float scale;

  /* This square sits strictly inside the limit circle. Most samples need no scaling. */
  if (peak <= 0.70f * limit)
  {
    return 0U;
  }
  dn = *d / peak;
  qn = *q / peak;
  scale = (limit / peak) / sqrtf((dn * dn) + (qn * qn));
  if (scale >= 1.0f)
  {
    return 0U;
  }
  *d *= scale;
  *q *= scale;
  return 1U;
}

void Foc_CurrentSetVbus(Foc_Current_t *cur, float vbus_v)
{
  if (cur == 0)
  {
    return;
  }
  if ((!isfinite(vbus_v)) || (vbus_v < CFG_VBUS_GAIN_MIN_V))
  {
    cur->vbus_v = 0.0f;
    cur->vbus_valid = 0U;
    cur->kp = 0.0f;
    cur->ki = 0.0f;
    Foc_CurrentReset(cur);
    return;
  }
  cur->vbus_v = vbus_v;
  cur->vbus_valid = 1U;
  cur->kp = cur->kp_v / vbus_v;
  cur->ki = cur->ki_v / vbus_v;
}

void Foc_CurrentInit(Foc_Current_t *cur, float r_ohm, float l_h, float bw_hz,
                     float vbus_v, float i_lim, float v_lim)
{
  float wc;

  if (cur == 0)
  {
    return;
  }
  memset(cur, 0, sizeof(*cur));
  if ((!isfinite(r_ohm)) || (!isfinite(l_h)) || (!isfinite(bw_hz)) ||
      (!isfinite(i_lim)) || (!isfinite(v_lim)) ||
      (r_ohm <= 0.0f) || (l_h <= 0.0f) || (bw_hz <= 0.0f) ||
      (i_lim <= 0.0f) || (v_lim <= 0.0f) || (v_lim > 1.0f))
  {
    return;
  }
  wc = FOC_TWO_PI * bw_hz;
  cur->kp_v = l_h * wc;
  cur->ki_v = r_ohm * wc;
  if ((!isfinite(cur->kp_v)) || (!isfinite(cur->ki_v)))
  {
    cur->kp_v = 0.0f;
    cur->ki_v = 0.0f;
    return;
  }
  cur->i_lim = i_lim;
  cur->v_lim = v_lim;
  if (CFG_I_LPF_HZ > 0.0f)
  {
    cur->i_lpf = 1.0f - expf(-FOC_TWO_PI * CFG_I_LPF_HZ * CFG_PWM_DT_S);
    if (!((cur->i_lpf > 0.0f) && (cur->i_lpf < 1.0f)))
    {
      cur->i_lpf = 1.0f;
    }
  }
  else
  {
    cur->i_lpf = 1.0f;
  }
  Foc_CurrentSetVbus(cur, vbus_v);
}

/**
 * @brief  滤波 Id/Iq。系数不小于 1 时直接赋值，保证关闭滤波的板卡与原来逐位一致。
 */
static void Foc_CurrentFilterDq(Foc_Current_t *cur, float id, float iq)
{
  if ((cur->i_lpf >= 1.0f) || (cur->i_lpf_ready == 0U))
  {
    cur->id = id;
    cur->iq = iq;
    cur->i_lpf_ready = 1U;
    return;
  }
  cur->id += cur->i_lpf * (id - cur->id);
  cur->iq += cur->i_lpf * (iq - cur->iq);
}

void Foc_CurrentReset(Foc_Current_t *cur)
{
  if (cur == 0)
  {
    return;
  }
  cur->vd = 0.0f;
  cur->vq = 0.0f;
  cur->id_i = 0.0f;
  cur->iq_i = 0.0f;
}

void Foc_CurrentStep(Foc_Current_t *cur, float ia, float ib, float elec_rad,
                     float id_ref, float iq_ref, float dt_s, uint8_t run_pi)
{
  Foc_Dq_t dq;
  float id_err;
  float iq_err;
  float vd_p;
  float vq_p;
  float id_next;
  float iq_next;
  float vd_request;
  float vq_request;
  float vd_limited;
  float vq_limited;
  float voltage_limit;

  if (cur == 0)
  {
    return;
  }
  if ((!isfinite(ia)) || (!isfinite(ib)) || (!isfinite(elec_rad)))
  {
    cur->ia = 0.0f;
    cur->ib = 0.0f;
    cur->id = 0.0f;
    cur->iq = 0.0f;
    cur->i_lpf_ready = 0U;
    Foc_CurrentReset(cur);
    return;
  }
  /* Keep the LUT's float-to-integer index conversion in range. Normal encoder
   * angles take the fast path, while other finite angles remain well-defined. */
  if ((elec_rad < 0.0f) || (elec_rad >= FOC_TWO_PI))
  {
    elec_rad = fmodf(elec_rad, FOC_TWO_PI);
    if (elec_rad < 0.0f)
    {
      elec_rad += FOC_TWO_PI;
    }
  }
  cur->ia = ia;
  cur->ib = ib;
  dq = Foc_Park(Foc_Clarke(ia, ib), elec_rad);
  if ((!isfinite(dq.d)) || (!isfinite(dq.q)))
  {
    cur->id = 0.0f;
    cur->iq = 0.0f;
    cur->i_lpf_ready = 0U;
    Foc_CurrentReset(cur);
    return;
  }
  Foc_CurrentFilterDq(cur, dq.d, dq.q);

  if ((run_pi == 0U) || (!isfinite(dt_s)) || (dt_s <= 0.0f) ||
      (!isfinite(id_ref)) || (!isfinite(iq_ref)) ||
      (cur->vbus_valid == 0U) || (!isfinite(cur->vbus_v)) ||
      (cur->vbus_v < CFG_VBUS_GAIN_MIN_V) ||
      (!isfinite(cur->kp_v)) || (!isfinite(cur->ki_v)) ||
      (cur->kp_v <= 0.0f) || (cur->ki_v <= 0.0f) ||
      (!isfinite(cur->i_lim)) || (cur->i_lim <= 0.0f) ||
      (!isfinite(cur->v_lim)) || (cur->v_lim <= 0.0f) ||
      (cur->v_lim > 1.0f) ||
      (!isfinite(cur->id_i)) || (!isfinite(cur->iq_i)))
  {
    Foc_CurrentReset(cur);
    return;
  }

  Foc_CurrentLimitVector(cur->i_lim, &id_ref, &iq_ref);
  id_err = id_ref - cur->id;
  iq_err = iq_ref - cur->iq;

  voltage_limit = cur->vbus_v * cur->v_lim;
  vd_p = cur->kp_v * id_err;
  vq_p = cur->kp_v * iq_err;
  id_next = cur->id_i + (cur->ki_v * id_err * dt_s);
  iq_next = cur->iq_i + (cur->ki_v * iq_err * dt_s);
  vd_request = vd_p + id_next;
  vq_request = vq_p + iq_next;
  if ((!isfinite(voltage_limit)) || (!isfinite(vd_request)) ||
      (!isfinite(vq_request)))
  {
    Foc_CurrentReset(cur);
    return;
  }

  vd_limited = vd_request;
  vq_limited = vq_request;
  if ((Foc_CurrentLimitVector(voltage_limit, &vd_limited, &vq_limited) == 0U) ||
      (((id_err * vd_limited) + (iq_err * vq_limited)) <= 0.0f))
  {
    /* Integrate while unsaturated, or when the error pulls a saturated
     * voltage vector inward. Freeze both axes when pushing it outward. */
    cur->id_i = id_next;
    cur->iq_i = iq_next;
  }
  /* Also bound stored volts when the measured supply falls. */
  Foc_CurrentLimitVector(voltage_limit, &cur->id_i, &cur->iq_i);
  vd_request = vd_p + cur->id_i;
  vq_request = vq_p + cur->iq_i;
  if ((!isfinite(vd_request)) || (!isfinite(vq_request)))
  {
    Foc_CurrentReset(cur);
    return;
  }
  Foc_CurrentLimitVector(voltage_limit, &vd_request, &vq_request);
  cur->vd = vd_request / cur->vbus_v;
  cur->vq = vq_request / cur->vbus_v;
}
