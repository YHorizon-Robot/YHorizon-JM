#include "adc.h"

#include "board.h"
#include "config.h"
#include "gd32f30x.h"
#include "ntc.h"

static volatile uint16_t s_vbus_raw;
static volatile float s_vbus_v;
static volatile uint32_t s_vbus_good_ms;
static volatile uint8_t s_vbus_valid;
static uint32_t s_last_poll_ms;
static uint16_t s_ia_offset;
static uint16_t s_ib_offset;
static uint8_t s_current_ready;
static uint8_t s_temp_present;
static volatile uint16_t s_temp_raw;
static volatile uint32_t s_temp_good_ms;
static volatile uint8_t s_temp_have;
static uint32_t s_temp_poll_ms;

/** @brief PA3 / ADC012_IN3。温度用 ADC1，母线固定在 ADC0 规则组 IN8。 */
#define ADC_TEMP_CHANNEL   ADC_CHANNEL_3
#define ADC_VBUS_CHANNEL   ADC_CHANNEL_8

static float Adc_RawToAmp(uint16_t raw, uint16_t offset)
{
  const float v = ((float)((int32_t)raw - (int32_t)offset) * CFG_ADC_VREF_V)
                  / CFG_ADC_FULLSCALE;
  return CFG_CURRENT_SENSE_SIGN * v / (CFG_CSA_GAIN * CFG_SHUNT_OHM);
}

static void Adc_StoreVbus(uint16_t raw)
{
  s_vbus_raw = raw;
  s_vbus_v = ((float)raw * CFG_ADC_VREF_V * CFG_VBUS_SCALE) / CFG_ADC_FULLSCALE;
  s_vbus_good_ms = Board_GetTickMs();
  s_vbus_valid = 1U;
}

/**
 * @brief  启动一次规则转换，并且只接受这次触发之后新出现的 EOC。
 *
 * 先读走数据寄存器再清标志。标志清不掉就返回失败，避免把上一个通道的结果当成这次的。
 */
static uint8_t Adc_ConvertOnce(uint16_t *raw_out)
{
  uint32_t guard = 200000U;

  /**
   * 电流中断也在清 ADC 标志。清一次可能清不掉，多丢几次再放弃，
   * 不要把这种失败当成 0 V。
   */
  {
    uint8_t attempt;
    for (attempt = 0U; attempt < 3U; attempt++)
    {
      (void)adc_routine_data_read(ADC0);
      adc_flag_clear(ADC0, ADC_FLAG_EOC);
      if (RESET == adc_flag_get(ADC0, ADC_FLAG_EOC))
      {
        break;
      }
    }
  }
  if (SET == adc_flag_get(ADC0, ADC_FLAG_EOC))
  {
    return 0U;
  }
  adc_software_trigger_enable(ADC0, ADC_ROUTINE_CHANNEL);
  while ((RESET == adc_flag_get(ADC0, ADC_FLAG_EOC)) && (--guard != 0U))
  {
  }
  if (guard == 0U)
  {
    return 0U;
  }
  *raw_out = adc_routine_data_read(ADC0);
  adc_flag_clear(ADC0, ADC_FLAG_EOC);
  return 1U;
}

/**
 * @brief  在 ADC1 上读一次 NTC。通道固定为 PA3 / IN3，不和 ADC0 的母线、相电流共用标志。
 */
static uint8_t Adc_PollTempOnce(uint16_t *raw_out)
{
  uint32_t guard = 200000U;

  (void)adc_routine_data_read(ADC1);
  adc_flag_clear(ADC1, ADC_FLAG_EOC);
  if (SET == adc_flag_get(ADC1, ADC_FLAG_EOC))
  {
    return 0U;
  }
  adc_software_trigger_enable(ADC1, ADC_ROUTINE_CHANNEL);
  while ((RESET == adc_flag_get(ADC1, ADC_FLAG_EOC)) && (--guard != 0U))
  {
  }
  if (guard == 0U)
  {
    return 0U;
  }
  *raw_out = adc_routine_data_read(ADC1);
  adc_flag_clear(ADC1, ADC_FLAG_EOC);
  return 1U;
}

/**
 * @brief  打开 ADC1，只采 PA3 上的 NTC。
 */
static void Adc_InitTemp(void)
{
  rcu_periph_clock_enable(RCU_ADC1);
  adc_deinit(ADC1);
  adc_data_alignment_config(ADC1, ADC_DATAALIGN_RIGHT);
  adc_channel_length_config(ADC1, ADC_ROUTINE_CHANNEL, 1U);
  adc_routine_channel_config(ADC1, 0U, ADC_TEMP_CHANNEL, ADC_SAMPLETIME_239POINT5);
  adc_external_trigger_source_config(ADC1, ADC_ROUTINE_CHANNEL, ADC0_1_2_EXTTRIG_ROUTINE_NONE);
  adc_external_trigger_config(ADC1, ADC_ROUTINE_CHANNEL, ENABLE);
  adc_enable(ADC1);
  {
    volatile uint32_t delay = 10000U;
    while (delay != 0U)
    {
      delay--;
    }
  }
  adc_calibration_enable(ADC1);
}

/**
 * @brief  上电探测 PA3 上有没有焊 NTC，判定见 Ntc_ProbeOk()。PWM 尚未启动时调用。
 */
static uint8_t Adc_ProbeTempSensor(void)
{
  uint16_t raw_low = 0U;
  uint16_t raw_high = 0U;

  gpio_bit_reset(GPIOA, GPIO_PIN_3);
  gpio_init(GPIOA, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_3);
  Board_DelayMs(1U);
  gpio_init(GPIOA, GPIO_MODE_AIN, GPIO_OSPEED_50MHZ, GPIO_PIN_3);
  Board_DelayMs(CFG_TEMP_PROBE_SETTLE_MS);
  if (Adc_PollTempOnce(&raw_low) == 0U)
  {
    return 0U;
  }

  gpio_bit_set(GPIOA, GPIO_PIN_3);
  gpio_init(GPIOA, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_3);
  Board_DelayMs(1U);
  gpio_init(GPIOA, GPIO_MODE_AIN, GPIO_OSPEED_50MHZ, GPIO_PIN_3);
  gpio_bit_reset(GPIOA, GPIO_PIN_3);
  Board_DelayMs(CFG_TEMP_PROBE_SETTLE_MS);
  if (Adc_PollTempOnce(&raw_high) == 0U)
  {
    return 0U;
  }
  return Ntc_ProbeOk(raw_low, raw_high);
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

  s_current_ready = 0U;

  adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                     ADC0_1_2_EXTTRIG_INSERTED_NONE);

  for (i = 0U; i < CFG_ADC_I_OFFSET_SAMPLES; i++)
  {
    if ((Adc_PollInsertedOnce(&raw_a, &raw_b) != 0U) &&
        (raw_a >= CFG_ADC_I_OFFSET_MIN) && (raw_a <= CFG_ADC_I_OFFSET_MAX) &&
        (raw_b >= CFG_ADC_I_OFFSET_MIN) && (raw_b <= CFG_ADC_I_OFFSET_MAX))
    {
      acc_a += raw_a;
      acc_b += raw_b;
      n++;
    }
  }

  if (n != 0U)
  {
    s_ia_offset = (uint16_t)(acc_a / n);
    s_ib_offset = (uint16_t)(acc_b / n);
  }
  s_current_ready = ((n != 0U) && (n == CFG_ADC_I_OFFSET_SAMPLES)) ? 1U : 0U;

  adc_flag_clear(ADC0, ADC_FLAG_EOIC);
  /* O3CPRE reaches TRGO before the channel/bridge output gates. Its PWM1
     rising edge occurs at the up-count compare, even with outputs off. */
  adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                     ADC0_1_EXTTRIG_INSERTED_T0_TRGO);
}

void Adc_Init(void)
{
  rcu_periph_clock_enable(RCU_GPIOA);
  rcu_periph_clock_enable(RCU_GPIOB);
  rcu_periph_clock_enable(RCU_ADC0);
  rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV8);

  gpio_init(GPIOA, GPIO_MODE_AIN, GPIO_OSPEED_50MHZ, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_3);
  gpio_init(GPIOB, GPIO_MODE_AIN, GPIO_OSPEED_50MHZ, GPIO_PIN_0);

  adc_deinit(ADC0);
  adc_mode_config(ADC_MODE_FREE);
  adc_data_alignment_config(ADC0, ADC_DATAALIGN_RIGHT);

  adc_channel_length_config(ADC0, ADC_ROUTINE_CHANNEL, 1U);
  adc_routine_channel_config(ADC0, 0U, ADC_VBUS_CHANNEL, ADC_SAMPLETIME_55POINT5);
  adc_external_trigger_source_config(ADC0, ADC_ROUTINE_CHANNEL, ADC0_1_2_EXTTRIG_ROUTINE_NONE);
  adc_external_trigger_config(ADC0, ADC_ROUTINE_CHANNEL, ENABLE);

  adc_channel_length_config(ADC0, ADC_INSERTED_CHANNEL, 2U);
  /* Two ranks need scan mode. At APB2/8, the second 1.5-cycle aperture
     ends 124 timer ticks after the sequence begins (1.5 + 12.5 + 1.5). */
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
  s_vbus_good_ms = 0U;
  s_vbus_valid = 0U;
  s_last_poll_ms = Board_GetTickMs() - CFG_VBUS_POLL_MS;
  s_ia_offset = 0U;
  s_ib_offset = 0U;
  s_temp_raw = 0U;
  s_temp_good_ms = 0U;
  s_temp_have = 0U;
  s_temp_poll_ms = Board_GetTickMs() - CFG_TEMP_POLL_MS;
  Adc_InitTemp();
  s_temp_present = Adc_ProbeTempSensor();
  Adc_CalibrateCurrentOffset();
  Adc_Service();
}

void Adc_Service(void)
{
  uint16_t raw;
  const uint32_t now = Board_GetTickMs();

  if ((now - s_last_poll_ms) < CFG_VBUS_POLL_MS)
  {
    return;
  }
  s_last_poll_ms = now;

  /** 这一拍没转成不抹掉上一笔有效电压，否则电流环会把 0 V 当成欠压。 */
  if (Adc_ConvertOnce(&raw) == 0U)
  {
    return;
  }

  Adc_StoreVbus(raw);

  if ((s_temp_present != 0U) && ((now - s_temp_poll_ms) >= CFG_TEMP_POLL_MS))
  {
    s_temp_poll_ms = now;
    if (Adc_PollTempOnce(&raw) != 0U)
    {
      s_temp_raw = raw;
      s_temp_good_ms = Board_GetTickMs();
      s_temp_have = 1U;
    }
  }
}

uint8_t Adc_TempSensorPresent(void)
{
  return s_temp_present;
}

uint8_t Adc_GetTempRaw(uint16_t *raw_out)
{
  if ((s_temp_present == 0U) || (s_temp_have == 0U) ||
      ((Board_GetTickMs() - s_temp_good_ms) > (5U * CFG_TEMP_POLL_MS)))
  {
    return 0U;
  }
  *raw_out = s_temp_raw;
  return 1U;
}

void Adc_RefreshVbus(void)
{
  s_last_poll_ms = Board_GetTickMs() - CFG_VBUS_POLL_MS;
  Adc_Service();
}

float Adc_GetVbusVolts(void)
{
  if ((s_vbus_valid == 0U) ||
      ((Board_GetTickMs() - s_vbus_good_ms) > CFG_VBUS_STALE_MS))
  {
    return 0.0f;
  }
  return s_vbus_v;
}

uint16_t Adc_GetVbusRaw(void)
{
  return s_vbus_raw;
}

void Adc_GetCurrentOffset(uint16_t *ia_offset, uint16_t *ib_offset)
{
  if (ia_offset != 0)
  {
    *ia_offset = s_ia_offset;
  }
  if (ib_offset != 0)
  {
    *ib_offset = s_ib_offset;
  }
}

uint8_t Adc_ReadPhaseCurrents(float *ia_a, float *ib_a)
{
  uint16_t raw_a;
  uint16_t raw_b;

  if ((ia_a == 0) || (ib_a == 0))
  {
    return 2U;
  }
  if (s_current_ready == 0U)
  {
    return 2U;
  }
  if (RESET == adc_flag_get(ADC0, ADC_FLAG_EOIC))
  {
    return 0U;
  }

  raw_a = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
  raw_b = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_1);
  adc_flag_clear(ADC0, ADC_FLAG_EOIC);
  if ((raw_a < CFG_ADC_I_RAW_MIN) || (raw_a > CFG_ADC_I_RAW_MAX) ||
      (raw_b < CFG_ADC_I_RAW_MIN) || (raw_b > CFG_ADC_I_RAW_MAX))
  {
    return 2U;
  }
  *ia_a = Adc_RawToAmp(raw_a, s_ia_offset);
  *ib_a = Adc_RawToAmp(raw_b, s_ib_offset);
  return 1U;
}

uint8_t Adc_CurrentSenseReady(void)
{
  return s_current_ready;
}
