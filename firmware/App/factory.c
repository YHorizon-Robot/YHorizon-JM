#include "factory.h"

#include "adc.h"
#include "board.h"
#include "clock_diag.h"
#include "config.h"
#include "factory_hw.h"
#include "debug.h"
#include "encoder.h"
#include "foc_math.h"
#include "pwm.h"
#include "servo.h"

#include <math.h>

/**
 * @name 故障位
 * @brief 置位后一直保持，RESULT 行按这些名字重复打印，方便 RTT 晚连上时还能看到。
 */
/** @{ */
#define F_XTAL_DEAD   (1UL << 0)
#define F_XTAL_SLOW   (1UL << 1)
#define F_XTAL_STOP   (1UL << 2)
#define F_PLL         (1UL << 3)
#define F_FREQ        (1UL << 4)
#define F_VBUS        (1UL << 5)
#define F_IOFF        (1UL << 6)
#define F_CAN         (1UL << 7)
#define F_ENC1        (1UL << 8)
#define F_ENC2        (1UL << 9)
#define F_MAG1        (1UL << 10)
#define F_MAG2        (1UL << 11)
#define F_CURRENT     (1UL << 12)
#define F_MOTION1     (1UL << 13)
#define F_MOTION2     (1UL << 14)
#define F_OC          (1UL << 15)
/** @} */

/**
 * @brief  认为“确实在 112 MHz 附近”的粗范围。
 *         112 MHz 配 30~60 kHz 的 IRC40K，估出来大约是 75~149 MHz。
 *         掉回 8 MHz 或 PLL 只乘到二十多 MHz 时，估算值会落在 40 MHz 以下。
 */
#define FACTORY_FREQ_MIN_HZ     (40000000UL)
#define FACTORY_FREQ_MAX_HZ     (180000000UL)

/**
 * @name MT6701 状态半字节
 * @brief 帧内 Mg[3:0]，与 Encoder 解析的 status 一致。
 */
/** @{ */
#define FACTORY_MG_TRACK        (0x08U)
#define FACTORY_MG_PUSH         (0x04U)
#define FACTORY_MG_FIELD_MASK   (0x03U)
#define FACTORY_MG_STRONG       (0x01U)
#define FACTORY_MG_WEAK         (0x02U)
/** @} */

typedef struct
{
  uint16_t raw;
  uint8_t status;
  uint8_t ok;
} FactoryEnc_t;

static uint32_t s_flags;
static uint8_t s_drag;
static uint8_t s_judged;
static uint8_t s_servo;
static uint8_t s_xtal_stop_printed;
static uint8_t s_oc_printed;
static uint16_t s_enc1_ok;
static uint16_t s_enc1_bad;
static uint16_t s_enc2_ok;
static uint16_t s_enc2_bad;
static uint8_t s_enc_judged;
static uint8_t s_mag1;
static uint8_t s_mag2;
static uint8_t s_have_base;
static uint16_t s_base1;
static uint16_t s_base2;
static uint32_t s_base_ms;
static float s_i_peak;
static uint32_t s_freq_hz;
static uint32_t s_drag_t0;
static float s_vbus;

static int Factory_Centi(float x)
{
  if (x >= 0.0f)
  {
    return (int)((x * 100.0f) + 0.5f);
  }
  return (int)((x * 100.0f) - 0.5f);
}

/**
 * @brief  把 MT6701 磁场状态译成短词。正常返回 "ok"。
 */
static const char *Factory_MagText(uint8_t st)
{
  if ((st & FACTORY_MG_TRACK) != 0U)
  {
    return "track-loss";
  }
  if ((st & FACTORY_MG_FIELD_MASK) == FACTORY_MG_STRONG)
  {
    return "strong";
  }
  if ((st & FACTORY_MG_FIELD_MASK) == FACTORY_MG_WEAK)
  {
    return "weak";
  }
  if ((st & FACTORY_MG_PUSH) != 0U)
  {
    return "push";
  }
  if ((st & FACTORY_MG_FIELD_MASK) == FACTORY_MG_FIELD_MASK)
  {
    return "field-rsv";
  }
  return "ok";
}

static uint8_t Factory_MagBad(uint8_t st)
{
  if ((st & (FACTORY_MG_TRACK | FACTORY_MG_PUSH)) != 0U)
  {
    return 1U;
  }
  if ((st & FACTORY_MG_FIELD_MASK) != 0U)
  {
    return 1U;
  }
  return 0U;
}

static const char *Factory_XtalTag(void)
{
  if (g_clock_diag.hxtal_lost != 0U)
  {
    return "STOP";
  }
  if (g_clock_diag.hxtal_ok == 0U)
  {
    return "DEAD";
  }
  if ((s_flags & F_XTAL_SLOW) != 0U)
  {
    return "SLOW";
  }
  if (g_clock_diag.pll_ok == 0U)
  {
    return "PLL";
  }
  if ((s_flags & F_FREQ) != 0U)
  {
    return "FREQ";
  }
  return "OK";
}

static void Factory_PrintResult(void)
{
  Dbg_Printf("RESULT %s", (s_flags == 0U) ? "PASS" : "FAIL");
  if ((s_flags & F_XTAL_DEAD) != 0U)
  {
    Dbg_Printf(" xtal-dead");
  }
  if ((s_flags & F_XTAL_SLOW) != 0U)
  {
    Dbg_Printf(" xtal-slow");
  }
  if ((s_flags & F_XTAL_STOP) != 0U)
  {
    Dbg_Printf(" xtal-stop");
  }
  if ((s_flags & F_PLL) != 0U)
  {
    Dbg_Printf(" pll");
  }
  if ((s_flags & F_FREQ) != 0U)
  {
    Dbg_Printf(" freq");
  }
  if ((s_flags & F_VBUS) != 0U)
  {
    Dbg_Printf(" vbus");
  }
  if ((s_flags & F_IOFF) != 0U)
  {
    Dbg_Printf(" ioff");
  }
  if ((s_flags & F_CAN) != 0U)
  {
    Dbg_Printf(" can");
  }
  if ((s_flags & F_ENC1) != 0U)
  {
    Dbg_Printf(" enc1");
  }
  if ((s_flags & F_ENC2) != 0U)
  {
    Dbg_Printf(" enc2");
  }
  if ((s_flags & F_MAG1) != 0U)
  {
    Dbg_Printf(" mag1=%s", Factory_MagText(s_mag1));
  }
  if ((s_flags & F_MAG2) != 0U)
  {
    Dbg_Printf(" mag2=%s", Factory_MagText(s_mag2));
  }
  if ((s_flags & F_CURRENT) != 0U)
  {
    Dbg_Printf(" current");
  }
  if ((s_flags & F_MOTION1) != 0U)
  {
    Dbg_Printf(" motion1");
  }
  if ((s_flags & F_MOTION2) != 0U)
  {
    Dbg_Printf(" motion2");
  }
  if ((s_flags & F_OC) != 0U)
  {
    Dbg_Printf(" oc");
  }
  Dbg_Printf("\r\n");
}

/**
 * @brief  晶振停振后硬件会切回 IRC8M。关掉 PWM，并按 8 MHz 重配 SysTick，RTT 仍可继续打。
 */
static void Factory_OnXtalLost(void)
{
  g_clock_diag.hxtal_lost = 1U;
  s_flags |= F_XTAL_STOP;
  if (s_drag != 0U)
  {
    Pwm_ApplyDuty(FOC_PWM_NEUTRAL_DUTY, FOC_PWM_NEUTRAL_DUTY, FOC_PWM_NEUTRAL_DUTY);
    Pwm_DisableOutputs();
    if (s_servo != 0U)
    {
      Servo_SetMode(SERVO_IDLE);
    }
    s_drag = 0U;
  }
  if (s_xtal_stop_printed == 0U)
  {
    FactoryHw_UseInternalClock();
    Dbg_Printf("xtal STOPPED (cold-joint or cracked crystal). PWM off\r\n");
    s_xtal_stop_printed = 1U;
    Factory_PrintResult();
  }
}

static void Factory_PollXtal(void)
{
  if (ClockDiag_PollLost() != 0U)
  {
    Factory_OnXtalLost();
  }
}

/**
 * @brief  上电时钟报告。慢起振算失败：虚焊常表现为起振变慢，或运行中被 CSS 抓住。
 */
static uint8_t Factory_CheckClock(void)
{
  const uint32_t start_us = ClockDiag_HxtalStartUs();
  const char *src = "IRC8M";
  uint8_t run_pll = 0U;

  s_freq_hz = 0U;
  if ((g_clock_diag.hxtal_ok != 0U) && (g_clock_diag.pll_ok != 0U) &&
      (g_clock_diag.sys_source == CLOCK_DIAG_SRC_PLL))
  {
    s_freq_hz = ClockDiag_MeasureHz();
  }

  if (g_clock_diag.sys_source == CLOCK_DIAG_SRC_PLL)
  {
    src = "PLL";
  }
  else if (g_clock_diag.sys_source == CLOCK_DIAG_SRC_HXTAL)
  {
    src = "HXTAL";
  }

  Dbg_Printf("xtal start=%uus hxtal=%u pll=%u src=%s\r\n",
             (unsigned)start_us,
             (unsigned)g_clock_diag.hxtal_ok,
             (unsigned)g_clock_diag.pll_ok,
             src);
  if (s_freq_hz == 0U)
  {
    Dbg_Printf("freq SKIP (IRC40K cross-check unavailable)\r\n");
  }
  else
  {
    Dbg_Printf("freq=%u.%uMHz (IRC40K coarse, expect ~%u)\r\n",
               (unsigned)(s_freq_hz / 1000000U),
               (unsigned)((s_freq_hz % 1000000U) / 100000U),
               (unsigned)(CFG_SYSCLK_HZ / 1000000UL));
  }

  if (g_clock_diag.hxtal_ok == 0U)
  {
    Dbg_Printf("xtal FAIL no-start (open, missing, or cold-joint)\r\n");
    s_flags |= F_XTAL_DEAD;
  }
  else if (start_us > CFG_FACTORY_XTAL_SLOW_US)
  {
    Dbg_Printf("xtal FAIL slow-start %uus > %uus (suspect cold-joint)\r\n",
               (unsigned)start_us,
               (unsigned)CFG_FACTORY_XTAL_SLOW_US);
    s_flags |= F_XTAL_SLOW;
  }
  else
  {
    Dbg_Printf("xtal start PASS\r\n");
  }

  if ((g_clock_diag.hxtal_ok != 0U) &&
      ((g_clock_diag.pll_ok == 0U) || (g_clock_diag.sys_source != CLOCK_DIAG_SRC_PLL)))
  {
    Dbg_Printf("pll FAIL (no lock, high-drive, or not selected)\r\n");
    s_flags |= F_PLL;
  }
  else if (g_clock_diag.pll_ok != 0U)
  {
    Dbg_Printf("pll PASS\r\n");
  }

  if ((s_freq_hz != 0U) &&
      ((s_freq_hz < FACTORY_FREQ_MIN_HZ) || (s_freq_hz > FACTORY_FREQ_MAX_HZ)))
  {
    Dbg_Printf("freq FAIL outside %u-%u MHz (not on the PLL)\r\n",
               (unsigned)(FACTORY_FREQ_MIN_HZ / 1000000U),
               (unsigned)(FACTORY_FREQ_MAX_HZ / 1000000U));
    s_flags |= F_FREQ;
  }
  else if (s_freq_hz != 0U)
  {
    Dbg_Printf("freq PASS\r\n");
  }

  run_pll = ((g_clock_diag.hxtal_ok != 0U) &&
             (g_clock_diag.pll_ok != 0U) &&
             (g_clock_diag.sys_source == CLOCK_DIAG_SRC_PLL) &&
             ((s_flags & F_FREQ) == 0U)) ? 1U : 0U;
  if (run_pll != 0U)
  {
    ClockDiag_EnableMonitor();
    Dbg_Printf("css ON (tap the crystal; a cold joint prints xtal STOPPED)\r\n");
  }
  return run_pll;
}

static uint8_t Factory_CheckVbus(void)
{
  uint16_t raw;

  Board_DelayMs(CFG_VBUS_POLL_MS + 1U);
  Adc_Service();
  raw = Adc_GetVbusRaw();
  s_vbus = Adc_GetVbusVolts();
  Dbg_Printf("vbus=%dcV raw=%u\r\n", Factory_Centi(s_vbus), (unsigned)raw);

  if ((raw < 100U) || (raw > 4000U) ||
      (s_vbus < CFG_FACTORY_VBUS_MIN_V) || (s_vbus > CFG_FACTORY_VBUS_MAX_V))
  {
    Dbg_Printf("vbus FAIL (expect %d-%d V, divider or supply)\r\n",
               (int)CFG_FACTORY_VBUS_MIN_V,
               (int)CFG_FACTORY_VBUS_MAX_V);
    s_flags |= F_VBUS;
    return 0U;
  }
  Dbg_Printf("vbus PASS\r\n");
  return 1U;
}

static void Factory_CheckOffset(void)
{
  uint16_t ia_off;
  uint16_t ib_off;

  Adc_GetCurrentOffset(&ia_off, &ib_off);
  Dbg_Printf("ioff ia=%u ib=%u\r\n", (unsigned)ia_off, (unsigned)ib_off);
  if ((ia_off < CFG_FACTORY_IOFF_MIN) || (ia_off > CFG_FACTORY_IOFF_MAX) ||
      (ib_off < CFG_FACTORY_IOFF_MIN) || (ib_off > CFG_FACTORY_IOFF_MAX))
  {
    Dbg_Printf("ioff FAIL (shunt, CSA, or PA0/PA1)\r\n");
    s_flags |= F_IOFF;
    return;
  }
  Dbg_Printf("ioff PASS\r\n");
}

/**
 * @brief  CAN 静默环回只证明控制器能自发自收，不覆盖收发器和总线。
 */
static void Factory_CheckCan(void)
{
  const uint8_t rc = FactoryHw_CanLoopback();

  if (rc == 1U)
  {
    Dbg_Printf("can FAIL init\r\n");
    s_flags |= F_CAN;
    return;
  }
  if (rc == 2U)
  {
    Dbg_Printf("can FAIL no mailbox\r\n");
    s_flags |= F_CAN;
    return;
  }
  if (rc == 3U)
  {
    Dbg_Printf("can FAIL loopback timeout\r\n");
    s_flags |= F_CAN;
    return;
  }
  if (rc == 4U)
  {
    Dbg_Printf("can FAIL loopback data\r\n");
    s_flags |= F_CAN;
    return;
  }
  Dbg_Printf("can PASS loopback (controller only, not the transceiver)\r\n");
}

static void Factory_ReadEnc(FactoryEnc_t *e1, FactoryEnc_t *e2)
{
  e1->status = 0U;
  e2->status = 0U;
  e1->ok = Encoder_ReadFrame(&e1->raw, &e1->status);
  e2->ok = Encoder2_ReadFrame(&e2->raw, &e2->status);

  if (e1->ok != 0U)
  {
    if (s_enc1_ok < 1000U)
    {
      s_enc1_ok++;
    }
    if (Factory_MagBad(e1->status) != 0U)
    {
      s_mag1 = e1->status;
      s_flags |= F_MAG1;
    }
  }
  else if (s_enc1_bad < 1000U)
  {
    s_enc1_bad++;
  }

  if (e2->ok != 0U)
  {
    if (s_enc2_ok < 1000U)
    {
      s_enc2_ok++;
    }
    if (Factory_MagBad(e2->status) != 0U)
    {
      s_mag2 = e2->status;
      s_flags |= F_MAG2;
    }
  }
  else if (s_enc2_bad < 1000U)
  {
    s_enc2_bad++;
  }
}

static void Factory_PrintEnc(const FactoryEnc_t *e1, const FactoryEnc_t *e2)
{
  if (e1->ok != 0U)
  {
    Dbg_Printf("enc1=%u st=%x %s ",
               (unsigned)e1->raw,
               (unsigned)e1->status,
               Factory_MagText(e1->status));
  }
  else
  {
    Dbg_Printf("enc1=NA ");
  }
  if (e2->ok != 0U)
  {
    Dbg_Printf("enc2=%u st=%x %s",
               (unsigned)e2->raw,
               (unsigned)e2->status,
               Factory_MagText(e2->status));
  }
  else
  {
    Dbg_Printf("enc2=NA");
  }
}

static uint32_t Factory_RawDelta(uint16_t a, uint16_t b)
{
  uint32_t d = ((uint32_t)b + ENCODER_CPR - (uint32_t)a) % ENCODER_CPR;
  if (d > (ENCODER_CPR / 2U))
  {
    d = ENCODER_CPR - d;
  }
  return d;
}

static void Factory_JudgeEnc(void);

static void Factory_NoteMotion(const FactoryEnc_t *e1, const FactoryEnc_t *e2, uint32_t now)
{
  if (s_drag == 0U)
  {
    return;
  }
  if ((e1->ok == 0U) || (e2->ok == 0U))
  {
    return;
  }
  if (s_have_base == 0U)
  {
    s_base1 = e1->raw;
    s_base2 = e2->raw;
    s_base_ms = now;
    s_have_base = 1U;
    return;
  }
  if ((s_judged != 0U) || ((now - s_base_ms) < CFG_FACTORY_MOTION_MS))
  {
    return;
  }

  {
    const uint32_t d1 = Factory_RawDelta(s_base1, e1->raw);
    const uint32_t d2 = Factory_RawDelta(s_base2, e2->raw);
    const float i_min = CFG_FACTORY_I_MIN_RATIO
                        * ((CFG_FACTORY_DRAG_V * s_vbus) / CFG_MOTOR_R_OHM);

    Factory_JudgeEnc();
    Dbg_Printf("motion d1=%u d2=%u ipeak=%dcA imin=%dcA\r\n",
               (unsigned)d1,
               (unsigned)d2,
               Factory_Centi(s_i_peak),
               Factory_Centi(i_min));
    if (d1 < CFG_FACTORY_MOTION_MIN_CNT)
    {
      Dbg_Printf("motion1 FAIL (motor not turning, or SPI0 encoder stuck)\r\n");
      s_flags |= F_MOTION1;
    }
    else
    {
      Dbg_Printf("motion1 PASS\r\n");
    }
    if (d2 < CFG_FACTORY_MOTION_MIN_CNT)
    {
      Dbg_Printf("motion2 FAIL (SPI2 / vernier encoder stuck)\r\n");
      s_flags |= F_MOTION2;
    }
    else
    {
      Dbg_Printf("motion2 PASS\r\n");
    }
    if ((s_flags & F_IOFF) != 0U)
    {
      Dbg_Printf("current SKIP bad offset\r\n");
    }
    else if ((s_flags & F_OC) == 0U)
    {
      if (s_i_peak < i_min)
      {
        Dbg_Printf("current FAIL (gate driver, motor, or shunt)\r\n");
        s_flags |= F_CURRENT;
      }
      else
      {
        Dbg_Printf("current PASS\r\n");
      }
    }
  }
  s_judged = 1U;
  Factory_PrintResult();
}

static void Factory_JudgeEnc(void)
{
  if (s_enc_judged != 0U)
  {
    return;
  }
  s_enc_judged = 1U;

  if (s_enc1_ok == 0U)
  {
    Dbg_Printf("enc1 FAIL no frame (SPI0, CS PA4, chip, or magnet)\r\n");
    s_flags |= F_ENC1;
  }
  else if (s_enc1_bad > s_enc1_ok)
  {
    Dbg_Printf("enc1 FAIL crc/spi\r\n");
    s_flags |= F_ENC1;
  }
  else
  {
    Dbg_Printf("enc1 link PASS\r\n");
  }

  if (s_enc2_ok == 0U)
  {
    Dbg_Printf("enc2 FAIL no frame (SPI2, CS PA15, chip, or magnet)\r\n");
    s_flags |= F_ENC2;
  }
  else if (s_enc2_bad > s_enc2_ok)
  {
    Dbg_Printf("enc2 FAIL crc/spi\r\n");
    s_flags |= F_ENC2;
  }
  else
  {
    Dbg_Printf("enc2 link PASS\r\n");
  }

  if ((s_flags & F_MAG1) != 0U)
  {
    Dbg_Printf("mag1 FAIL %s\r\n", Factory_MagText(s_mag1));
  }
  if ((s_flags & F_MAG2) != 0U)
  {
    Dbg_Printf("mag2 FAIL %s\r\n", Factory_MagText(s_mag2));
  }
}

static void Factory_PollCurrent(void)
{
  ServoTelemetry_t tel;
  float ia;
  float ib;

  if ((s_drag == 0U) || (s_servo == 0U) || ((s_flags & F_IOFF) != 0U))
  {
    return;
  }
  Servo_GetTelemetry(&tel);
  ia = fabsf(tel.ia);
  ib = fabsf(tel.ib);
  if (ia > s_i_peak)
  {
    s_i_peak = ia;
  }
  if (ib > s_i_peak)
  {
    s_i_peak = ib;
  }
  if ((ia < CFG_FACTORY_I_OC_A) && (ib < CFG_FACTORY_I_OC_A))
  {
    return;
  }

  Pwm_ApplyDuty(FOC_PWM_NEUTRAL_DUTY, FOC_PWM_NEUTRAL_DUTY, FOC_PWM_NEUTRAL_DUTY);
  Pwm_DisableOutputs();
  Servo_SetMode(SERVO_IDLE);
  s_drag = 0U;
  s_flags |= F_OC;
  if (s_oc_printed == 0U)
  {
    Dbg_Printf("oc FAIL ia=%dcA ib=%dcA. PWM off\r\n",
               Factory_Centi(tel.ia),
               Factory_Centi(tel.ib));
    s_oc_printed = 1U;
    Factory_JudgeEnc();
    s_judged = 1U;
    Factory_PrintResult();
  }
}

static void Factory_StartDrag(void)
{
  const int rate_mrad = (int)((CFG_FACTORY_DRAG_ELEC_RAD_S * 1000.0f) + 0.5f);

  Servo_Init();
  s_servo = 1U;
  Pwm_StartTimer();
  Pwm_EnableOutputs();
  Servo_SetOpenloop(CFG_FACTORY_DRAG_V, 0.0f, CFG_FACTORY_DRAG_ELEC_RAD_S, 0.0f);
  s_drag = 1U;
  s_drag_t0 = Board_GetTickMs();
  Dbg_Printf("drag ON Vd_centi=%d rate=%d mrad/s elec\r\n",
             Factory_Centi(CFG_FACTORY_DRAG_V),
             rate_mrad);
}

void Factory_Run(void)
{
  uint32_t last_print = 0U;
  uint32_t last_result = 0U;
  uint32_t last_led = 0U;
  uint8_t clock_ok;
  uint8_t vbus_ok;
  FactoryEnc_t e1;
  FactoryEnc_t e2;

  Board_Init();
  Dbg_Init();
  Dbg_Printf("\r\n==== YHorizon-JM factory ====\r\n");
  Dbg_Printf("openloop Vd centi-pu=%d (100 = 1.00*Vbus), two MT6701\r\n",
             Factory_Centi(CFG_FACTORY_DRAG_V));

  clock_ok = Factory_CheckClock();
  vbus_ok = Factory_CheckVbus();
  Factory_CheckOffset();
  Factory_CheckCan();

  Factory_ReadEnc(&e1, &e2);
  Dbg_Printf("enc ");
  Factory_PrintEnc(&e1, &e2);
  Dbg_Printf("\r\n");

  if ((clock_ok != 0U) && (vbus_ok != 0U))
  {
    Factory_StartDrag();
  }
  else
  {
    Dbg_Printf("drag OFF (clock or vbus). encoders still polled\r\n");
    Factory_JudgeEnc();
    s_judged = 1U;
    Factory_PrintResult();
  }

  for (;;)
  {
    uint32_t now;

    Adc_Service();
    Factory_PollXtal();
    Factory_PollCurrent();
    now = Board_GetTickMs();

    if ((now - last_led) >= ((s_flags == 0U) ? 400U : 80U))
    {
      last_led = now;
      Board_LedToggle();
    }

    if ((now - last_print) < CFG_FACTORY_PRINT_MS)
    {
      continue;
    }
    last_print = now;

    Factory_ReadEnc(&e1, &e2);
    s_vbus = Adc_GetVbusVolts();
    Factory_PrintEnc(&e1, &e2);
    if (s_servo != 0U)
    {
      ServoTelemetry_t tel;
      Servo_GetTelemetry(&tel);
      Dbg_Printf(" ia=%d ib=%d", Factory_Centi(tel.ia), Factory_Centi(tel.ib));
    }
    Dbg_Printf(" vbus=%d key=%u xtal=%s\r\n",
               Factory_Centi(s_vbus),
               (unsigned)Board_ButtonRaw(),
               Factory_XtalTag());

    if (s_drag != 0U)
    {
      Factory_NoteMotion(&e1, &e2, now);
    }
    if ((s_judged == 0U) && (s_drag != 0U))
    {
      uint8_t timed_out = 0U;

      if (s_have_base != 0U)
      {
        if ((now - s_base_ms) >= (CFG_FACTORY_MOTION_MS + 1500U))
        {
          timed_out = 1U;
        }
      }
      else if ((now - s_drag_t0) >= 2000U)
      {
        timed_out = 1U;
      }
      if (timed_out != 0U)
      {
        Factory_JudgeEnc();
        Dbg_Printf("motion FAIL timeout (encoder pair never stayed up)\r\n");
        s_flags |= F_MOTION1 | F_MOTION2;
        s_judged = 1U;
        Factory_PrintResult();
      }
    }
    if ((s_judged == 0U) && (s_drag == 0U))
    {
      Factory_JudgeEnc();
      if ((s_flags & (F_MOTION1 | F_MOTION2 | F_CURRENT)) == 0U)
      {
        Dbg_Printf("motion SKIP drag-off\r\n");
      }
      s_judged = 1U;
    }
    if ((s_judged != 0U) && ((now - last_result) >= 2000U))
    {
      last_result = now;
      Factory_PrintResult();
    }
  }
}
