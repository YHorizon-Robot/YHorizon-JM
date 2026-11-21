#ifndef CURRENT_SAMPLE_TIMING_H
#define CURRENT_SAMPLE_TIMING_H

#include <stdint.h>

/**
 * Select an up-count compare near the carrier peak. Active-high PWM1/PWM0
 * high sides are off above their compare values, so all low sides conduct
 * from max(CCR) to 2*ARR-max(CCR). Include dead time/CSA settling in blank_ticks
 * and trigger synchronization plus both sampling apertures in sample_ticks.
 * Return zero when the configured modulation leaves no guaranteed window.
 */
static inline uint16_t CurrentSample_Compare(uint32_t arr, float v_limit,
                                           uint32_t blank_ticks, uint32_t sample_ticks)
{
  uint32_t edge;
  uint32_t lo;
  uint32_t hi;
  uint32_t compare;

  if ((arr < 2U) || (arr > 65535U) ||
      !(v_limit >= 0.0f && v_limit <= 0.57735026919f) ||
      (blank_ticks >= arr) || (sample_ticks >= arr))
  {
    return 0U;
  }
  /* One count beyond the rounded PWM compare is conservative for every angle. */
  edge = (uint32_t)((0.5f + 0.86602540378f * v_limit) * (float)arr) + 1U;
  lo = edge + blank_ticks;
  hi = 2U * arr - edge - sample_ticks;
  if (hi >= arr)
  {
    hi = arr - 1U;
  }
  if (lo > hi)
  {
    return 0U;
  }
  compare = arr - sample_ticks / 2U;
  if (compare < lo)
  {
    compare = lo;
  }
  else if (compare > hi)
  {
    compare = hi;
  }
  return (uint16_t)compare;
}

#endif /* CURRENT_SAMPLE_TIMING_H */
