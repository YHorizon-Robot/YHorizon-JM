#include "nv_flash.h"

#include "config.h"
#include "gd32f30x.h"

#include <string.h>

uint32_t NvFlash_PageAddr(void)
{
  return BOARD_NV_PAGE_ADDR;
}

void NvFlash_Read(uint32_t addr, void *dst, uint32_t len)
{
  const uint8_t *src = (const uint8_t *)addr;
  uint8_t *out = (uint8_t *)dst;
  uint32_t i;

  if ((dst == 0) || (len == 0U))
  {
    return;
  }

  __DSB();
  for (i = 0U; i < len; i++)
  {
    out[i] = src[i];
  }
  __DSB();
}

/**
 * @brief  擦写期间屏蔽中断。恢复前清掉 PWM、运控和 CAN 的挂起请求，
 *         避免擦写期间积压的中断在开中断后连着进。
 */
static uint32_t NvFlash_IrqQuiesce(void)
{
  const uint32_t primask = __get_PRIMASK();

  __disable_irq();
  __DSB();
  __ISB();
  return primask;
}

static void NvFlash_IrqResume(uint32_t primask)
{
  timer_interrupt_flag_clear(TIMER0, TIMER_INT_FLAG_UP);
  timer_interrupt_flag_clear(TIMER5, TIMER_INT_FLAG_UP);
  NVIC_ClearPendingIRQ(TIMER0_UP_IRQn);
  NVIC_ClearPendingIRQ(TIMER5_IRQn);
  NVIC_ClearPendingIRQ(USBD_LP_CAN0_RX0_IRQn);
  NVIC_ClearPendingIRQ(CAN0_EWMC_IRQn);
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
  __DSB();
  __ISB();
  __set_PRIMASK(primask);
}

static uint8_t NvFlash_WriteAt(uint32_t addr, const void *data, uint32_t len)
{
  uint32_t i;
  uint8_t ok = 0U;
  uint32_t primask;
  const uint32_t nwords = len / 4U;

  if ((data == 0) || (len == 0U) || ((len & 3U) != 0U) || (len > 2048U))
  {
    return 0U;
  }

  primask = NvFlash_IrqQuiesce();
  fmc_unlock();
  if (FMC_READY == fmc_page_erase(addr))
  {
    ok = 1U;
    for (i = 0U; i < nwords; i++)
    {
      uint32_t word;
      memcpy(&word, (const uint8_t *)data + i * 4U, sizeof(word));
      if (FMC_READY != fmc_word_program(addr + (i * 4UL), word))
      {
        ok = 0U;
        break;
      }
    }
  }
  fmc_lock();
  __DSB();
  NvFlash_IrqResume(primask);
  return ok;
}

uint8_t NvFlash_WritePage(const void *data, uint32_t len)
{
  return (len <= 64U) ? NvFlash_WriteAt(BOARD_NV_PAGE_ADDR, data, len) : 0U;
}

uint8_t NvFlash_WriteCommissionPage(uint8_t slot, const void *data, uint32_t len)
{
  return (slot < 2U) ? NvFlash_WriteAt(BOARD_NV_PAGE_ADDR - (2U - slot) * 2048U, data, len) : 0U;
}
