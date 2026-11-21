#include "pwm.h"
#include "config.h"
#include "foc_math.h"
#include "gd32f30x.h"

#if CFG_CURRENT_EARLY_PWM
/* Allow time for all three APB writes once the counter has been checked. */
#ifndef CFG_PWM_EARLY_GUARD_US
#define CFG_PWM_EARLY_GUARD_US (1U)
#endif
#define PWM_EARLY_GUARD_TICKS ((BOARD_APB2_HZ / 1000000UL) * CFG_PWM_EARLY_GUARD_US)

static uint32_t s_deferred[3];
static uint8_t s_deferred_valid;
volatile uint32_t g_pwm_early_updates;
volatile uint32_t g_pwm_deferred_updates;
volatile uint32_t g_pwm_deferred_applied;
volatile uint32_t g_pwm_early_min_ticks = UINT32_MAX;
#endif

static uint32_t Pwm_DutyToCompare(float duty)
{
  const uint32_t arr = TIMER_CAR(TIMER0);
  duty = Foc_Clamp(duty, 0.0f, 1.0f);
  return (uint32_t)((duty * (float)arr) + 0.5f);
}

void Pwm_StartTimer(void)
{
  const uint32_t mid = (CFG_TIM1_ARR + 1U) / 2U;
  TIMER_CH0CV(TIMER0) = mid;
  TIMER_CH1CV(TIMER0) = mid;
  TIMER_CH2CV(TIMER0) = mid;
  /* Load all CCR shadows and RCR=1 before starting at zero/up. The first
     completed update is then the valley, after the peak's ADC sequence. */
  timer_event_software_generate(TIMER0, TIMER_EVENT_SRC_UPG);
  TIMER_CNT(TIMER0) = 0U;
  TIMER_CTL0(TIMER0) &= ~TIMER_CTL0_DIR;
  timer_interrupt_flag_clear(TIMER0, TIMER_INT_FLAG_UP);
  timer_interrupt_enable(TIMER0, TIMER_INT_UP);
#if CFG_CURRENT_EARLY_PWM
  /* Offset calibration runs before the timer, so its software triggers
   * remain polled. Match the valley IRQ priority to serialize both paths. */
  adc_flag_clear(ADC0, ADC_FLAG_EOIC);
  nvic_irq_enable(ADC0_1_IRQn, CFG_NVIC_PWM, 0U);
  ADC_CTL0(ADC0) |= ADC_CTL0_EOICIE;
#endif
  timer_enable(TIMER0);
}

void Pwm_EnableOutputs(void)
{
  timer_channel_output_state_config(TIMER0, TIMER_CH_0, TIMER_CCX_ENABLE);
  timer_channel_output_state_config(TIMER0, TIMER_CH_1, TIMER_CCX_ENABLE);
  timer_channel_output_state_config(TIMER0, TIMER_CH_2, TIMER_CCX_ENABLE);
  timer_channel_complementary_output_state_config(TIMER0, TIMER_CH_0, TIMER_CCXN_ENABLE);
  timer_channel_complementary_output_state_config(TIMER0, TIMER_CH_1, TIMER_CCXN_ENABLE);
  timer_channel_complementary_output_state_config(TIMER0, TIMER_CH_2, TIMER_CCXN_ENABLE);
  timer_primary_output_config(TIMER0, ENABLE);
}

void Pwm_DisableOutputs(void)
{
  timer_primary_output_config(TIMER0, DISABLE);
#if CFG_CURRENT_EARLY_PWM
  s_deferred_valid = 0U;
#endif
  timer_channel_output_state_config(TIMER0, TIMER_CH_0, TIMER_CCX_DISABLE);
  timer_channel_output_state_config(TIMER0, TIMER_CH_1, TIMER_CCX_DISABLE);
  timer_channel_output_state_config(TIMER0, TIMER_CH_2, TIMER_CCX_DISABLE);
  timer_channel_complementary_output_state_config(TIMER0, TIMER_CH_0, TIMER_CCXN_DISABLE);
  timer_channel_complementary_output_state_config(TIMER0, TIMER_CH_1, TIMER_CCXN_DISABLE);
  timer_channel_complementary_output_state_config(TIMER0, TIMER_CH_2, TIMER_CCXN_DISABLE);
}

void Pwm_ApplyDuty(float duty_a, float duty_b, float duty_c)
{
#if CFG_CURRENT_EARLY_PWM
  s_deferred_valid = 0U;
#endif
  TIMER_CH0CV(TIMER0) = Pwm_DutyToCompare(duty_a);
  TIMER_CH1CV(TIMER0) = Pwm_DutyToCompare(duty_b);
  TIMER_CH2CV(TIMER0) = Pwm_DutyToCompare(duty_c);
}

#if CFG_CURRENT_EARLY_PWM
void Pwm_ApplyDutyEarly(float duty_a, float duty_b, float duty_c)
{
  const uint32_t a = Pwm_DutyToCompare(duty_a);
  const uint32_t b = Pwm_DutyToCompare(duty_b);
  const uint32_t c = Pwm_DutyToCompare(duty_c);
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  const uint32_t count = TIMER_CNT(TIMER0);
  /* During up-counting the valley is at least a half-period away. The
   * update flag also covers a wrap just before reading the counter. */
  const uint32_t remaining = (TIMER_CTL0(TIMER0) & TIMER_CTL0_DIR)
                             ? count : TIMER_CAR(TIMER0);
  if (!(TIMER_INTF(TIMER0) & TIMER_INT_FLAG_UP) && remaining > PWM_EARLY_GUARD_TICKS)
  {
    TIMER_CH0CV(TIMER0) = a;
    TIMER_CH1CV(TIMER0) = b;
    TIMER_CH2CV(TIMER0) = c;
    __set_PRIMASK(primask);
    g_pwm_early_updates++;
    if (remaining < g_pwm_early_min_ticks) { g_pwm_early_min_ticks = remaining; }
    return;
  }
  __set_PRIMASK(primask);
  s_deferred[0] = a;
  s_deferred[1] = b;
  s_deferred[2] = c;
  s_deferred_valid = 1U;
  g_pwm_deferred_updates++;
}

void Pwm_ApplyDeferredDuty(void)
{
  if (s_deferred_valid == 0U) { return; }
  s_deferred_valid = 0U;
  if (!(TIMER_CCHP(TIMER0) & TIMER_CCHP_POEN)) { return; }
  TIMER_CH0CV(TIMER0) = s_deferred[0];
  TIMER_CH1CV(TIMER0) = s_deferred[1];
  TIMER_CH2CV(TIMER0) = s_deferred[2];
  g_pwm_deferred_applied++;
}
#endif

void CtrlTimer_Start(void)
{
  timer_interrupt_flag_clear(TIMER5, TIMER_INT_FLAG_UP);
  timer_interrupt_enable(TIMER5, TIMER_INT_UP);
  timer_enable(TIMER5);
}
