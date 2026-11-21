#include "cali.h"

#include "adc.h"
#include "board.h"
#include "config.h"
#include "debug.h"
#include "foc_vernier.h"
#include "pwm.h"
#include "servo.h"
#if (CFG_COMMISSION != 0U)
#include "commission.h"
#endif

static uint8_t EncoderReady(void)
{
  const uint32_t start = Board_GetTickMs();
  while (!Servo_EncoderStable())
  {
    if (Board_GetTickMs() - start >= CFG_ENC_STARTUP_MS || Servo_GetFault()) { return 0U; }
    Board_DelayMs(1U);
  }
  return Foc_EncoderIsReady(Servo_GetEncoder()) && Servo_GetFault() == 0U;
}

/* Loading an alignment never opens the power bridge. */
uint8_t Cali_ApplySaved(const CaliNvData_t *nv)
{
  if (!EncoderReady()) { return 0U; }
  const uint32_t lock = Board_IrqLock();
  Servo_SetPolePairs(nv->pole_pairs);
  Servo_SetEncoderAlignment(nv->encoder_dir, nv->electrical_offset_rad);
  Servo_SetClosedLoopDir(nv->closed_loop_dir);
  Foc_VernierSetOffset(nv->enc2_offset_valid ? (float)nv->enc2_offset_raw / 256.0f : 0.0f);
  Board_IrqUnlock(lock);

  if (!Servo_SeedMultiTurn(nv->mt_zero_rad))
  {
    Servo_SeedFallback(Foc_EncoderGetLastRaw(Servo_GetEncoder()), nv->mt_zero_rad);
    Dbg_Printf("cali: vernier fallback\r\n");
  }

  Adc_RefreshVbus();
  const uint16_t scale = Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT
      ? (uint16_t)((CFG_I_LIMIT_A / CFG_V_LIMIT) * 1000.0f + 0.5f) : 1000U;
  if (nv->user_gains_valid && nv->user_gains_inner_loop == Servo_GetInnerLoop() &&
      nv->user_gains_scale_milli == scale)
  {
    Servo_SetVelocityGains((float)nv->vel_kp_raw * CFG_KP_VEL_LSB,
                           (float)nv->vel_ki_raw * CFG_KI_VEL_LSB);
    Servo_SetPositionGains((float)nv->pos_kp_raw * CFG_KP_POS_LSB,
                           (float)nv->pos_ki_raw * CFG_KI_POS_LSB,
                           (float)nv->pos_kd_raw * CFG_KD_POS_LSB);
  }
  if (Servo_GetFault()) { return 0U; }
  Servo_ResumeAfterFlash(SERVO_RUN);
  CtrlTimer_Start();
  return 1U;
}

uint8_t Cali_Start(void)
{
  CaliNvData_t nv;
  uint8_t valid = CaliNv_Load(&nv);
#if (CFG_COMMISSION != 0U)
  valid = Commission_MergeAlignment(&nv, valid);
#endif
  if (!valid)
  {
    Dbg_Printf("cali: waiting for host\r\n");
    return 0U;
  }
  if (!Cali_ApplySaved(&nv)) { return 0U; }
#if (CFG_COMMISSION != 0U)
  Commission_ApplyControl();
#endif
  return 1U;
}
