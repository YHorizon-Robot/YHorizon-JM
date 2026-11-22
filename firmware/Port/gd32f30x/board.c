#include "board.h"
#include "adc.h"
#include "clock_diag.h"
#include "config.h"
#include "current_sample_timing.h"
#include "gd32f30x.h"

static volatile uint32_t s_tick_ms;

#if CFG_RS05 || CFG_CYBERGEAR
/* First two words of the GD32F30x electronic signature. */
void Board_GetUid64(uint8_t uid[8])
{
  const volatile uint32_t *signature = (const volatile uint32_t *)0x1FFFF7E8U;
  for (unsigned word = 0; word < 2; ++word)
  {
    uint32_t bits = signature[word];
    for (unsigned i = 0; i < 4; ++i) { uid[word * 4 + i] = (uint8_t)(bits >> (8U*i)); }
  }
}
#endif

static void Board_GpioInit(void)
{
  rcu_periph_clock_enable(RCU_GPIOA);
  rcu_periph_clock_enable(RCU_GPIOB);
  rcu_periph_clock_enable(RCU_GPIOC);
  rcu_periph_clock_enable(RCU_AF);

  /* Free PA15 / PB3 / PB4 for SPI2; keep SWD on PA13/PA14. */
  gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);

  /* LED_STA: PC13 sinks LED1 (active low). Idle high = off. */
  gpio_bit_set(GPIOC, GPIO_PIN_13);
  gpio_init(GPIOC, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_13);

  gpio_bit_set(GPIOA, GPIO_PIN_4 | GPIO_PIN_15);
  gpio_init(GPIOA, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_4 | GPIO_PIN_15);

  /* KEY on PB1, external 5.1 kΩ pull-up, pressed to GND. */
  gpio_init(GPIOB, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, GPIO_PIN_1);
}

/**
 * @brief  取不超过 sclk_max_hz 的最快分频。
 */
static uint32_t Board_SpiPrescale(uint32_t pclk_hz, uint32_t sclk_max_hz)
{
  static const uint32_t divs[8] = {2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U};
  static const uint32_t psc[8] = {
    SPI_PSC_2, SPI_PSC_4, SPI_PSC_8, SPI_PSC_16,
    SPI_PSC_32, SPI_PSC_64, SPI_PSC_128, SPI_PSC_256
  };
  uint32_t i;

  for (i = 0U; i < 8U; i++)
  {
    if ((pclk_hz / divs[i]) <= sclk_max_hz)
    {
      return psc[i];
    }
  }
  return SPI_PSC_256;
}

static void Board_SpiFillCommon(spi_parameter_struct *spi_init_struct)
{
  spi_struct_para_init(spi_init_struct);
  spi_init_struct->trans_mode = SPI_TRANSMODE_FULLDUPLEX;
  spi_init_struct->device_mode = SPI_MASTER;
  spi_init_struct->nss = SPI_NSS_SOFT;
  spi_init_struct->endian = SPI_ENDIAN_MSB;
#if (CFG_ENCODER_TYPE == CFG_ENCODER_KTH7812)
  spi_init_struct->frame_size = SPI_FRAMESIZE_16BIT;
  spi_init_struct->clock_polarity_phase = SPI_CK_PL_HIGH_PH_2EDGE;
#else
  spi_init_struct->frame_size = SPI_FRAMESIZE_8BIT;
  spi_init_struct->clock_polarity_phase = SPI_CK_PL_LOW_PH_2EDGE;
#endif
}

static void Board_SpiInit(void)
{
  spi_parameter_struct spi_init_struct;

  rcu_periph_clock_enable(RCU_GPIOA);
  rcu_periph_clock_enable(RCU_GPIOB);
  rcu_periph_clock_enable(RCU_AF);
  rcu_periph_clock_enable(RCU_SPI0);
  rcu_periph_clock_enable(RCU_SPI2);

  gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_5 | GPIO_PIN_7);
  gpio_init(GPIOB, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_3 | GPIO_PIN_5);
#if (CFG_ENCODER_TYPE == CFG_ENCODER_KTH7812)
  gpio_init(GPIOA, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, GPIO_PIN_6);
  gpio_init(GPIOB, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, GPIO_PIN_4);
#else
  gpio_init(GPIOA, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_6);
  gpio_init(GPIOB, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_4);
#endif

  Board_SpiFillCommon(&spi_init_struct);
  spi_init_struct.prescale = Board_SpiPrescale(BOARD_APB2_HZ, CFG_ENC_SCLK_MAX_HZ);
  spi_i2s_deinit(SPI0);
  spi_init(SPI0, &spi_init_struct);
  spi_enable(SPI0);

  Board_SpiFillCommon(&spi_init_struct);
  spi_init_struct.prescale = Board_SpiPrescale(BOARD_APB1_HZ, CFG_ENC_SCLK_MAX_HZ);
  spi_i2s_deinit(SPI2);
  spi_init(SPI2, &spi_init_struct);
  spi_enable(SPI2);
}

static void Board_CanGpioInit(void)
{
  rcu_periph_clock_enable(RCU_GPIOB);
  rcu_periph_clock_enable(RCU_AF);
  rcu_periph_clock_enable(RCU_CAN0);

  /* Default CAN0 is PA11/PA12 (USB D-/D+). Partial remap → PB8/PB9. */
  gpio_pin_remap_config(GPIO_CAN_PARTIAL_REMAP, ENABLE);

  gpio_init(GPIOB, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, GPIO_PIN_8);
  gpio_init(GPIOB, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_9);
}

static void Board_Tim0Init(void)
{
  timer_parameter_struct timer_initpara;
  timer_oc_parameter_struct timer_ocinitpara;
  timer_break_parameter_struct timer_breakpara;
  uint16_t pulse_50;

  rcu_periph_clock_enable(RCU_GPIOA);
  rcu_periph_clock_enable(RCU_GPIOB);
  rcu_periph_clock_enable(RCU_AF);
  rcu_periph_clock_enable(RCU_TIMER0);

  gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
            GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10);
  gpio_init(GPIOB, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
            GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);

  timer_deinit(TIMER0);
  timer_struct_para_init(&timer_initpara);
  timer_initpara.prescaler = 0U;
  timer_initpara.alignedmode = TIMER_COUNTER_CENTER_UP;
  timer_initpara.counterdirection = TIMER_COUNTER_UP;
  timer_initpara.period = (uint16_t)CFG_TIM1_ARR;
  timer_initpara.clockdivision = TIMER_CKDIV_DIV1;
  timer_initpara.repetitioncounter = 1U;
  timer_init(TIMER0, &timer_initpara);

  timer_channel_output_struct_para_init(&timer_ocinitpara);
  timer_ocinitpara.outputstate = TIMER_CCX_DISABLE;
  timer_ocinitpara.outputnstate = TIMER_CCXN_DISABLE;
  timer_ocinitpara.ocpolarity = TIMER_OC_POLARITY_HIGH;
  timer_ocinitpara.ocnpolarity = TIMER_OCN_POLARITY_HIGH;
  timer_ocinitpara.ocidlestate = TIMER_OC_IDLE_STATE_LOW;
  timer_ocinitpara.ocnidlestate = TIMER_OCN_IDLE_STATE_LOW;

  pulse_50 = (uint16_t)((CFG_TIM1_ARR + 1U) / 2U);
  timer_channel_output_config(TIMER0, TIMER_CH_0, &timer_ocinitpara);
  timer_channel_output_config(TIMER0, TIMER_CH_1, &timer_ocinitpara);
  timer_channel_output_config(TIMER0, TIMER_CH_2, &timer_ocinitpara);
  timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_0, pulse_50);
  timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_1, pulse_50);
  timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, pulse_50);
  timer_channel_output_mode_config(TIMER0, TIMER_CH_0, TIMER_OC_MODE_PWM0);
  timer_channel_output_mode_config(TIMER0, TIMER_CH_1, TIMER_OC_MODE_PWM0);
  timer_channel_output_mode_config(TIMER0, TIMER_CH_2, TIMER_OC_MODE_PWM0);
  timer_channel_output_shadow_config(TIMER0, TIMER_CH_0, TIMER_OC_SHADOW_ENABLE);
  timer_channel_output_shadow_config(TIMER0, TIMER_CH_1, TIMER_OC_SHADOW_ENABLE);
  timer_channel_output_shadow_config(TIMER0, TIMER_CH_2, TIMER_OC_SHADOW_ENABLE);

  /* PWM1 makes O3CPRE rise at the up-count compare, once per carrier period.
     Route this pre-gate reference through TRGO so sampling continues with
     channel/bridge outputs disabled. Sample near the peak, after all low
     sides have turned on. Allow three ADC clocks for the external trigger
     synchronizer, beyond the two sampling apertures. */
  {
    const uint16_t trig = CurrentSample_Compare(CFG_TIM1_ARR, CFG_V_LIMIT,
                                               CFG_ADC_I_BLANK_TICKS,
                                               CFG_ADC_I_SAMPLE_END_TICKS + 24U);
    if (trig == 0U)
    {
      Error_Handler();
    }
    timer_channel_output_config(TIMER0, TIMER_CH_3, &timer_ocinitpara);
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_3, trig);
    timer_channel_output_mode_config(TIMER0, TIMER_CH_3, TIMER_OC_MODE_PWM1);
    timer_channel_output_shadow_config(TIMER0, TIMER_CH_3, TIMER_OC_SHADOW_ENABLE);
    timer_channel_output_state_config(TIMER0, TIMER_CH_3, TIMER_CCX_DISABLE);
    timer_master_output_trigger_source_select(TIMER0, TIMER_TRI_OUT_SRC_O3CPRE);
  }

  timer_break_struct_para_init(&timer_breakpara);
  timer_breakpara.runoffstate = TIMER_ROS_STATE_ENABLE;
  timer_breakpara.ideloffstate = TIMER_IOS_STATE_ENABLE;
  timer_breakpara.deadtime = (uint16_t)CFG_TIM1_DEADTIME_DTG;
  timer_breakpara.breakpolarity = TIMER_BREAK_POLARITY_HIGH;
  timer_breakpara.outputautostate = TIMER_OUTAUTO_DISABLE;
  timer_breakpara.protectmode = TIMER_CCHP_PROT_OFF;
  timer_breakpara.breakstate = TIMER_BREAK_DISABLE;
  timer_break_config(TIMER0, &timer_breakpara);

  timer_auto_reload_shadow_enable(TIMER0);
  timer_primary_output_config(TIMER0, DISABLE);

  nvic_irq_enable(TIMER0_UP_IRQn, CFG_NVIC_PWM, 0U);
}

static void Board_Tim5Init(void)
{
  timer_parameter_struct timer_initpara;

  rcu_periph_clock_enable(RCU_TIMER5);
  timer_deinit(TIMER5);
  timer_struct_para_init(&timer_initpara);
  timer_initpara.prescaler = (uint16_t)CFG_CTRL_TIM_PSC;
  timer_initpara.alignedmode = TIMER_COUNTER_EDGE;
  timer_initpara.counterdirection = TIMER_COUNTER_UP;
  timer_initpara.period = (uint16_t)CFG_CTRL_TIM_ARR;
  timer_initpara.clockdivision = TIMER_CKDIV_DIV1;
  timer_initpara.repetitioncounter = 0U;
  timer_init(TIMER5, &timer_initpara);

  nvic_irq_enable(TIMER5_IRQn, CFG_NVIC_CTRL, 0U);
}

static void Board_SystickInit(void)
{
  if (SysTick_Config(SystemCoreClock / 1000U) != 0U)
  {
    Error_Handler();
  }
  NVIC_SetPriority(SysTick_IRQn, 15U);
}

void Board_Init(void)
{
#if (CFG_FACTORY_TEST == 0U)
  /* Only the factory test can run on the IRC8M fallback. Servo timing and
     CAN bit timing require the board's configured PLL clock. */
  if ((g_clock_diag.hxtal_ok == 0U) ||
      (g_clock_diag.pll_ok == 0U) ||
      (g_clock_diag.sys_source != CLOCK_DIAG_SRC_PLL))
  {
    Error_Handler();
  }
#endif
  nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0);
  Board_SystickInit();
  Board_GpioInit();
  Board_CanGpioInit();
  Board_SpiInit();
  Board_Tim0Init();
  Board_Tim5Init();
  Board_DwtInit();
  Adc_Init();
}

void Board_DwtInit(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t Board_DwtGetCycles(void)
{
  return DWT->CYCCNT;
}

uint32_t Board_DwtCyclesToNs(uint32_t cycles)
{
  return (uint32_t)(((uint64_t)cycles * 1000000000ULL) / (uint64_t)CFG_SYSCLK_HZ);
}

void Board_LedSet(uint8_t on)
{
  gpio_bit_write(GPIOC, GPIO_PIN_13, (on != 0U) ? RESET : SET);
}

void Board_LedToggle(void)
{
  gpio_bit_write(GPIOC, GPIO_PIN_13,
                 (RESET == gpio_output_bit_get(GPIOC, GPIO_PIN_13)) ? SET : RESET);
}

uint8_t Board_ButtonRaw(void)
{
  return (RESET == gpio_input_bit_get(GPIOB, GPIO_PIN_1)) ? 1U : 0U;
}

void Board_IncTick(void)
{
  s_tick_ms++;
}

uint32_t Board_GetTickMs(void)
{
  return s_tick_ms;
}

void Board_DelayMs(uint32_t ms)
{
  const uint32_t start = s_tick_ms;
  while ((s_tick_ms - start) < ms)
  {
  }
}

uint32_t Board_IrqLock(void)
{
  const uint32_t primask = __get_PRIMASK();

  __disable_irq();
  return primask;
}

void Board_IrqUnlock(uint32_t primask)
{
  __set_PRIMASK(primask);
}

void Board_SystemReset(void)
{
  NVIC_SystemReset();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
