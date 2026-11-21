#ifndef CLOCK_DIAG_H
#define CLOCK_DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  SystemInit 仍在 IRC8M（8 MHz）上等待晶振时的 DWT 周期上限。
 *         100 ms 内 HXTALSTB 不置位视为未起振。
 */
#define CLOCK_DIAG_HXTAL_FAIL_CYC   (800000UL)
/**
 * @brief  PLL 锁定 / 高驱 / 时钟切换的等待上限，约 10 ms @ 8 MHz。
 */
#define CLOCK_DIAG_PLL_FAIL_CYC     (80000UL)

#define CLOCK_DIAG_SRC_IRC8M        (0U)
#define CLOCK_DIAG_SRC_HXTAL        (1U)
#define CLOCK_DIAG_SRC_PLL          (2U)

/**
 * @brief  上电时钟诊断。hxtal_cycles 在 IRC8M 下计数，8 个周期约 1 us。
 */
typedef struct
{
  uint32_t hxtal_cycles;
  uint8_t hxtal_ok;
  uint8_t pll_ok;
  uint8_t sys_source;
  volatile uint8_t hxtal_lost;
} ClockDiag_t;

extern ClockDiag_t g_clock_diag;

/**
 * @brief  记录 SystemInit 的晶振/PLL 结果。未切到 PLL 时把 SystemCoreClock 改成 IRC8M，
 *         避免后续 SysTick 按 112 MHz 配置却实际跑在 8 MHz 上。
 */
void ClockDiag_BootSet(uint32_t hxtal_cycles, uint8_t hxtal_ok, uint8_t pll_ok, uint8_t sys_source);

/**
 * @brief  HXTAL 起振耗时，单位微秒。时基是起振阶段的 IRC8M。
 */
uint32_t ClockDiag_HxtalStartUs(void);

/**
 * @brief  用 IRC40K 驱动 RTC，对照 DWT 粗测当前 SYSCLK。
 *         IRC40K 规格大约 30~60 kHz，只能区分“大约 112 MHz”和“掉回 8 MHz / PLL 没乘上去”。
 *         测不到时返回 0，不代表晶振一定坏。
 */
uint32_t ClockDiag_MeasureHz(void);

/**
 * @brief  查询晶振是否已经丢失（时钟监视或 HXTALSTB 掉下去）。
 * @return 1 已丢失。
 */
uint8_t ClockDiag_PollLost(void);

/**
 * @brief  打开晶振时钟监视。丢失时进入 NMI。
 */
void ClockDiag_EnableMonitor(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_DIAG_H */
