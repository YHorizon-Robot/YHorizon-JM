#ifndef FOC_CURRENT_H
#define FOC_CURRENT_H

#include <stdint.h>
#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  float ia;
  float ib;
  float id;
  float iq;
  float vd;
  float vq;
  /* Integrator outputs in volts, independent of bus-voltage scaling. */
  float id_i;
  float iq_i;
  /* Normalized gains retained for diagnostics; the PI uses kp_v/ki_v. */
  float kp;
  float ki;
  float kp_v;
  float ki_v;
  float i_lim;
  float v_lim;
  float vbus_v;
  uint8_t vbus_valid;
  /**
   * @brief Id/Iq 一阶低通系数。1 表示直通。
   */
  float i_lpf;
  /** @brief 已采到第一帧电流后才开始滤波，避免从上一次的 0 慢慢爬。 */
  uint8_t i_lpf_ready;
} Foc_Current_t;

void Foc_CurrentInit(Foc_Current_t *cur, float r_ohm, float l_h, float bw_hz,
                     float vbus_v, float i_lim, float v_lim);

/**
 * @brief  Update measured Vbus and normalized gains, preserving integrator volts.
 *         Invalid/below-minimum Vbus clears vbus_valid and resets the PI.
 *         Init may use nominal Vbus; the caller must supply valid measured Vbus
 *         before enabling closed-loop outputs.
 */
void Foc_CurrentSetVbus(Foc_Current_t *cur, float vbus_v);
void Foc_CurrentReset(Foc_Current_t *cur);

/**
 * @brief  Clarke/Park; optionally close Id/Iq PI with radial voltage saturation
 *         and conditional integration. References are limited as a vector.
 *         Nonfinite inputs, invalid dt/gains/Vbus reset the PI to zero output.
 * @param  run_pi  0: observe only and clear integrators. 1: PI → vd/vq.
 */
void Foc_CurrentStep(Foc_Current_t *cur, float ia, float ib, float elec_rad,
                     float id_ref, float iq_ref, float dt_s, uint8_t run_pi);

#ifdef __cplusplus
}
#endif

#endif /* FOC_CURRENT_H */
