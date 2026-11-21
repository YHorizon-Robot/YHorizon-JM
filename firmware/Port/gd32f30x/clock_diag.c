#include "clock_diag.h"

#include "gd32f30x.h"
#include "gd32f30x_rtc.h"

ClockDiag_t g_clock_diag;

/**
 * @brief  IRC40K 经 RTC 分频后的节拍数。30 拍约 30 ms，足够把轮询误差淹没。
 */
#define CLOCK_DIAG_RTC_PSC          (39UL)
#define CLOCK_DIAG_RTC_TICKS        (30UL)

void ClockDiag_BootSet(uint32_t hxtal_cycles, uint8_t hxtal_ok, uint8_t pll_ok, uint8_t sys_source)
{
  g_clock_diag.hxtal_cycles = hxtal_cycles;
  g_clock_diag.hxtal_ok = hxtal_ok;
  g_clock_diag.pll_ok = pll_ok;
  g_clock_diag.sys_source = sys_source;
  if (sys_source != CLOCK_DIAG_SRC_PLL)
  {
    SystemCoreClock = IRC8M_VALUE;
  }
}

uint32_t ClockDiag_HxtalStartUs(void)
{
  return g_clock_diag.hxtal_cycles / (IRC8M_VALUE / 1000000UL);
}

/**
 * @brief  等待 RTC_CTL 中的某一位置位。RTC 内核时钟停掉时会超时返回，避免卡死。
 */
static uint8_t ClockDiag_RtcWait(uint32_t mask)
{
  uint32_t guard = 2000000U;

  while (((RTC_CTL & mask) == 0U) && (guard != 0U))
  {
    guard--;
  }
  return (guard != 0U) ? 1U : 0U;
}

/**
 * @brief  把 RTC 配成 IRC40K/(PSC+1) 的自由计数器，并完成一次 APB 同步。
 */
static uint8_t ClockDiag_RtcStart(uint32_t psc)
{
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }
  RTC_CTL |= RTC_CTL_CMF;
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }

  RTC_PSCH = (psc >> 16) & 0x0FU;
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }
  RTC_PSCL = psc & 0xFFFFU;
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }

  RTC_CNTH = 0U;
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }
  RTC_CNTL = 0U;
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }

  RTC_CTL &= ~RTC_CTL_CMF;
  if (ClockDiag_RtcWait(RTC_CTL_LWOFF) == 0U)
  {
    return 0U;
  }

  RTC_CTL &= ~RTC_CTL_RSYNF;
  return ClockDiag_RtcWait(RTC_CTL_RSYNF);
}

uint32_t ClockDiag_MeasureHz(void)
{
  uint32_t c0;
  uint32_t c1;
  uint32_t t0;
  uint32_t t1;
  uint32_t delta;
  uint32_t guard;
  uint32_t tick_hz;

  rcu_periph_clock_enable(RCU_PMU);
  rcu_periph_clock_enable(RCU_BKPI);
  pmu_backup_write_enable();
  rcu_bkp_reset_enable();
  rcu_bkp_reset_disable();
  pmu_backup_write_enable();

  rcu_osci_on(RCU_IRC40K);
  if (SUCCESS != rcu_osci_stab_wait(RCU_IRC40K))
  {
    return 0U;
  }

  rcu_rtc_clock_config(RCU_RTCSRC_IRC40K);
  rcu_periph_clock_enable(RCU_RTC);

  t0 = DWT->CYCCNT;
  while ((DWT->CYCCNT - t0) < (SystemCoreClock / 1000U))
  {
  }

  if (ClockDiag_RtcStart(CLOCK_DIAG_RTC_PSC) == 0U)
  {
    return 0U;
  }

  c0 = RTC_CNTL & 0xFFFFU;
  t0 = DWT->CYCCNT;
  guard = 0U;
  do
  {
    c1 = RTC_CNTL & 0xFFFFU;
    guard++;
  } while (((uint32_t)(c1 - c0) < CLOCK_DIAG_RTC_TICKS) && (guard < 20000000U));

  t1 = DWT->CYCCNT;
  delta = (uint32_t)(c1 - c0);
  if (delta < CLOCK_DIAG_RTC_TICKS)
  {
    return 0U;
  }

  tick_hz = IRC40K_VALUE / (CLOCK_DIAG_RTC_PSC + 1UL);
  return (uint32_t)(((uint64_t)(t1 - t0) * (uint64_t)tick_hz) / (uint64_t)delta);
}

uint8_t ClockDiag_PollLost(void)
{
  if (SET == rcu_interrupt_flag_get(RCU_INT_FLAG_CKM))
  {
    rcu_interrupt_flag_clear(RCU_INT_FLAG_CKM_CLR);
    g_clock_diag.hxtal_lost = 1U;
  }
  if (g_clock_diag.hxtal_lost != 0U)
  {
    return 1U;
  }
  if ((g_clock_diag.hxtal_ok != 0U) && (RESET == rcu_flag_get(RCU_FLAG_HXTALSTB)))
  {
    g_clock_diag.hxtal_lost = 1U;
    return 1U;
  }
  return 0U;
}

void ClockDiag_EnableMonitor(void)
{
  rcu_hxtal_clock_monitor_enable();
}
