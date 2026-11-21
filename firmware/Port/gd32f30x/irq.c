#include "board.h"
#include "can.h"
#include "clock_diag.h"
#include "config.h"
#include "foc_math.h"
#include "pwm.h"
#include "servo.h"
#include "gd32f30x.h"
#include "SEGGER_RTT.h"

void NMI_Handler(void)
{
  if (SET == rcu_interrupt_flag_get(RCU_INT_FLAG_CKM))
  {
    g_clock_diag.hxtal_lost = 1U;
    rcu_hxtal_clock_monitor_disable();
    rcu_interrupt_flag_clear(RCU_INT_FLAG_CKM_CLR);
    if ((RCU_APB2EN & RCU_APB2EN_TIMER0EN) != 0U)
    {
      Pwm_ApplyDuty(FOC_PWM_NEUTRAL_DUTY, FOC_PWM_NEUTRAL_DUTY, FOC_PWM_NEUTRAL_DUTY);
      Pwm_DisableOutputs();
    }
  }
}

void HardFault_Handler(void)
{
  SEGGER_RTT_Init();
  SEGGER_RTT_WriteString(0, "HARDFAULT\r\n");
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
  Board_IncTick();
}

#if CFG_CURRENT_EARLY_PWM
static uint8_t s_current_handled;

void ADC0_1_IRQHandler(void)
{
  if (!(ADC_STAT(ADC0) & ADC_STAT_EOIC)) { return; }
  /* Keep EOIC for the sample reader. If the early path is ineligible or
   * the update already arrived, the valley handler will consume it. */
  ADC_CTL0(ADC0) &= ~ADC_CTL0_EOICIE;
  if (!s_current_handled && !(TIMER_INTF(TIMER0) & TIMER_INT_FLAG_UP))
  {
    s_current_handled = Servo_OnCurrentSampleIsr();
  }
}
#endif

void TIMER0_UP_IRQHandler(void)
{
  if (SET == timer_interrupt_flag_get(TIMER0, TIMER_INT_FLAG_UP))
  {
    timer_interrupt_flag_clear(TIMER0, TIMER_INT_FLAG_UP);
#if CFG_CURRENT_EARLY_PWM
    if (s_current_handled)
    {
      Pwm_ApplyDeferredDuty();
      Servo_OnPwmEncoderIsr();
      s_current_handled = 0U;
    }
    else
    {
      Servo_OnPwmIsr();
    }
    /* Do not clear EOIC here: a new conversion can arrive during the ISR. */
    ADC_CTL0(ADC0) |= ADC_CTL0_EOICIE;
#else
    Servo_OnPwmIsr();
#endif
  }
}

void TIMER5_IRQHandler(void)
{
  if (SET == timer_interrupt_flag_get(TIMER5, TIMER_INT_FLAG_UP))
  {
    timer_interrupt_flag_clear(TIMER5, TIMER_INT_FLAG_UP);
    Servo_OnCtrlIsr();
  }
}

void USBD_LP_CAN0_RX0_IRQHandler(void)
{
  Can_ProcessRxIrq();
}

void CAN0_EWMC_IRQHandler(void)
{
  Can_OnBusOffIrq();
}
