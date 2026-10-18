#include "adc.h"

#include "board.h"
#include "config.h"

static volatile uint16_t s_vbus_raw;
static volatile float s_vbus_v;
static uint32_t s_last_poll_ms;
static uint16_t s_ia_offset;
static uint16_t s_ib_offset;

static float Adc_RawToAmp(uint16_t raw, uint16_t offset)
{
  const float v = ((float)((int32_t)raw - (int32_t)offset) * CFG_ADC_VREF_V)
                  / CFG_ADC_FULLSCALE;
  return v / (CFG_CSA_GAIN * CFG_SHUNT_OHM);
}

static uint8_t Adc_PollRegularOnce(uint16_t *raw_out)
{
  uint32_t guard = 20000U;

  adc_flag_clear(ADC0, ADC_FLAG_EOC);
  adc_software_trigger_enable(ADC0, ADC_ROUTINE_CHANNEL);
  while ((RESET == adc_flag_get(ADC0, ADC_FLAG_EOC)) && (--guard != 0U))
  {
  }
  if (guard == 0U)
  {
    return 0U;
  }

  *raw_out = adc_routine_data_read(ADC0);
  return 1U;
}

static uint8_t Adc_PollInsertedOnce(uint16_t *raw_a, uint16_t *raw_b)
{
  uint32_t guard = 20000U;

  adc_flag_clear(ADC0, ADC_FLAG_EOIC);
  adc_software_trigger_enable(ADC0, ADC_INSERTED_CHANNEL);
  while ((RESET == adc_flag_get(ADC0, ADC_FLAG_EOIC)) && (--guard != 0U))
  {
  }
  if (guard == 0U)
  {
    return 0U;
  }

  *raw_a = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
  *raw_b = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_1);
  adc_flag_clear(ADC0, ADC_FLAG_EOIC);
  return 1U;
}

static void Adc_CalibrateCurrentOffset(void)
{
  uint32_t acc_a = 0U;
  uint32_t acc_b = 0U;
  uint32_t n = 0U;
  uint32_t i;
  uint16_t raw_a;
  uint16_t raw_b;

  adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                     ADC0_1_2_EXTTRIG_INSERTED_NONE);

  for (i = 0U; i < CFG_ADC_I_OFFSET_SAMPLES; i++)
  {
    if (Adc_PollInsertedOnce(&raw_a, &raw_b) != 0U)
    {
      acc_a += raw_a;
      acc_b += raw_b;
      n++;
    }
  }

  if (n == 0U)
  {
    s_ia_offset = 2048U;
    s_ib_offset = 2048U;
  }
  else
  {
    s_ia_offset = (uint16_t)(acc_a / n);
    s_ib_offset = (uint16_t)(acc_b / n);
  }

  adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                     ADC0_1_EXTTRIG_INSERTED_T0_CH3);
}

void Adc_Init(void)
{
  rcu_periph_clock_enable(RCU_GPIOA);
  rcu_periph_clock_enable(RCU_GPIOB);
  rcu_periph_clock_enable(RCU_ADC0);
  rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV8);

  gpio_init(GPIOA, GPIO_MODE_AIN, GPIO_OSPEED_50MHZ, GPIO_PIN_0 | GPIO_PIN_1);
  gpio_init(GPIOB, GPIO_MODE_AIN, GPIO_OSPEED_50MHZ, GPIO_PIN_0);

  adc_deinit(ADC0);
  adc_mode_config(ADC_MODE_FREE);
  adc_data_alignment_config(ADC0, ADC_DATAALIGN_RIGHT);

  adc_channel_length_config(ADC0, ADC_ROUTINE_CHANNEL, 1U);
  adc_routine_channel_config(ADC0, 0U, ADC_CHANNEL_8, ADC_SAMPLETIME_55POINT5);
  adc_external_trigger_source_config(ADC0, ADC_ROUTINE_CHANNEL, ADC0_1_2_EXTTRIG_ROUTINE_NONE);
  adc_external_trigger_config(ADC0, ADC_ROUTINE_CHANNEL, ENABLE);

  adc_channel_length_config(ADC0, ADC_INSERTED_CHANNEL, 2U);
  /**
   * 1.5-cycle sampling matches @ref CFG_ADC_I_SAMPLE_END_TICKS. A longer
   * aperture does not fit the low-side window at @ref CFG_V_LIMIT.
   */
  adc_inserted_channel_config(ADC0, 0U, ADC_CHANNEL_0, ADC_SAMPLETIME_1POINT5);
  adc_inserted_channel_config(ADC0, 1U, ADC_CHANNEL_1, ADC_SAMPLETIME_1POINT5);
  adc_special_function_config(ADC0, ADC_SCAN_MODE, ENABLE);
  adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                     ADC0_1_2_EXTTRIG_INSERTED_NONE);
  adc_external_trigger_config(ADC0, ADC_INSERTED_CHANNEL, ENABLE);

  adc_enable(ADC0);
  {
    volatile uint32_t delay = 10000U;
    while (delay != 0U)
    {
      delay--;
    }
  }
  adc_calibration_enable(ADC0);

  s_vbus_raw = 0U;
  s_vbus_v = 0.0f;
  s_last_poll_ms = 0U;
  s_ia_offset = 2048U;
  s_ib_offset = 2048U;
  Adc_CalibrateCurrentOffset();
  Adc_Service();
}

void Adc_Service(void)
{
  uint16_t raw;
  const uint32_t now = HAL_GetTick();

  if ((now - s_last_poll_ms) < CFG_VBUS_POLL_MS)
  {
    return;
  }
  s_last_poll_ms = now;

  if (Adc_PollRegularOnce(&raw) == 0U)
  {
    return;
  }

  s_vbus_raw = raw;
  s_vbus_v = ((float)raw * CFG_ADC_VREF_V * CFG_VBUS_SCALE) / CFG_ADC_FULLSCALE;
}

float Adc_GetVbusVolts(void)
{
  return s_vbus_v;
}

uint16_t Adc_GetVbusRaw(void)
{
  return s_vbus_raw;
}

uint8_t Adc_ReadPhaseCurrents(float *ia_a, float *ib_a)
{
  uint16_t raw_a;
  uint16_t raw_b;

  if ((ia_a == 0) || (ib_a == 0))
  {
    return 0U;
  }
  if (RESET == adc_flag_get(ADC0, ADC_FLAG_EOIC))
  {
    return 0U;
  }

  raw_a = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
  raw_b = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_1);
  adc_flag_clear(ADC0, ADC_FLAG_EOIC);
  *ia_a = Adc_RawToAmp(raw_a, s_ia_offset);
  *ib_a = Adc_RawToAmp(raw_b, s_ib_offset);
  return 1U;
}
