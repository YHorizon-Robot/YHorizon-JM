#include "servo.h"
#include "multi_axis.h"

#include "adc.h"
#include "config.h"
#include "encoder.h"
#include "foc_current.h"
#include "foc_math.h"
#include "foc_svpwm.h"
#include "foc_vernier.h"
#include "ntc.h"
#include "pwm.h"
#include "board.h"
#if (CFG_COMMISSION != 0U)
#include "commission.h"
#endif

#include <math.h>

static Foc_Encoder_t s_enc;
static Foc_Current_t s_cur;
static volatile ServoMode_t s_mode = SERVO_IDLE;
static volatile uint8_t s_ctrl_mode = SERVO_CTRL_MOTION;
static volatile uint8_t s_inner_loop = CFG_INNER_LOOP;
static volatile uint8_t s_cmd_valid = 0U;

static volatile float s_p_set;
static volatile float s_v_set;
static volatile float s_v_max;
static volatile float s_kp;
static volatile float s_kd;
static volatile float s_t_ff;
static volatile float s_t_ref;
static volatile float s_vel_kp;
static volatile float s_vel_ki;
static volatile float s_pos_kp;
static volatile float s_pos_ki;
static volatile float s_pos_kd;
static volatile int8_t s_closed_loop_dir = 1;

static float s_vel_i;
static float s_pos_i;

static volatile float s_ol_d;
static volatile float s_ol_q;
static volatile float s_ol_rate;
static volatile float s_ol_elec;

static volatile float s_volt_d;
static volatile float s_volt_q;
static float s_d_out;
static float s_q_out;
static volatile uint16_t s_enc1_raw;
static volatile uint8_t s_enc1_ok;
static float s_ia;
static float s_ib;
static float s_iq_ref;
static volatile uint16_t s_fault;
static uint8_t s_current_missing;
static uint8_t s_current_valid;
static uint8_t s_encoder_missing;
static volatile uint8_t s_encoder_good;
static uint8_t s_command_outputs_disabled;
static uint8_t s_neutral_updates;
static volatile uint8_t s_enable_blank;
static uint8_t s_oc_hits;
static uint16_t s_uv_hits;
static volatile uint8_t s_warning;

#if (BOARD_HAS_ENCODER2 != 0U)
/** @brief 上电定圈：连续多少次解码一致才采信，最多读多少次（间隔 1 ms）。 */
#define SERVO_VERNIER_SEED_HITS    (5U)
#define SERVO_VERNIER_SEED_TRIES   (20U)
/** @brief 定圈失败后补定圈：关桥且无指令时每毫秒读一次，连续多少次解码一致才采信。 */
#define SERVO_VERNIER_LATE_HITS    (20U)
#define SERVO_VERNIER_PERIOD_RAD   ((float)FOC_VERNIER_MOTOR_TURNS * FOC_TWO_PI)

static volatile uint8_t s_abs_valid;
static uint8_t s_vernier_late_hits;
static int32_t s_vernier_late_turns;
static uint32_t s_vernier_late_ms;
#endif

static uint8_t s_temp_valid;
static float s_temp_c;
static uint8_t s_temp_hot;
static uint8_t s_temp_over;
static uint32_t s_temp_ms;

void Servo_ResetFeedbackGains(void)
{
  /* Current torque gain differs from the old voltage controller. Require
   * explicit velocity/position tuning instead of silently reusing it. */
  s_vel_kp = (s_inner_loop == CFG_INNER_LOOP_CURRENT) ? 0.0f : CFG_VEL_KP_DEFAULT;
  s_vel_ki = (s_inner_loop == CFG_INNER_LOOP_CURRENT) ? 0.0f : CFG_VEL_KI_DEFAULT;
  s_pos_kp = CFG_POS_KP_DEFAULT;
  s_pos_ki = CFG_POS_KI_DEFAULT;
  s_pos_kd = CFG_POS_KD_DEFAULT;
}

static void Servo_ResetOuterGains(void)
{
  s_kp = 0.0f;
  s_kd = 0.0f;
  s_t_ff = 0.0f;
  Servo_ResetFeedbackGains();
}

static void Servo_ClearLoopState(void)
{
  s_vel_i = 0.0f;
  s_pos_i = 0.0f;
  s_t_ref = 0.0f;
  s_cmd_valid = 0U;
  s_iq_ref = 0.0f;
}

static void Servo_Trip(uint16_t fault)
{
  s_fault |= fault;
  s_mode = SERVO_FAULT;
  Pwm_DisableOutputs();
  s_command_outputs_disabled = 1U;
  s_neutral_updates = 2U;
  s_enable_blank = 0U;
  s_oc_hits = 0U;
  s_uv_hits = 0U;
  Servo_ClearLoopState();
  Foc_CurrentReset(&s_cur);
  s_d_out = 0.0f;
  s_q_out = 0.0f;
  Pwm_ApplyDuty(0.5f, 0.5f, 0.5f);
}

static float Servo_PiLimited(float *i, float err, float kp, float ki, float dt, float lim)
{
  *i += ki * err * dt;
  *i = Foc_Clamp(*i, -lim, lim);
  return Foc_Clamp((kp * err) + *i, -lim, lim);
}

static float Servo_PosSpeedLimit(float ep, float vmax, float acc)
{
  float lim = vmax;

  if ((acc > 0.0f) && (lim > 0.0f))
  {
    const float v_dec = sqrtf(2.0f * acc * fabsf(ep));
    if (v_dec < lim)
    {
      lim = v_dec;
    }
  }
  return lim;
}

static float Servo_Slew(float current, float target, float max_step)
{
  const float delta = target - current;

  if (delta > max_step)
  {
    return current + max_step;
  }
  if (delta < -max_step)
  {
    return current - max_step;
  }
  return target;
}

static float Servo_MotorToOut(float motor_rad)
{
  return motor_rad * CFG_GEAR_RATIO_INV;
}

#if (BOARD_HAS_ENCODER2 != 0U)
/**
 * @brief  把角度差折到 (-period/2, period/2]。
 */
static float Servo_WrapSym(float x, float period)
{
  return x - (period * floorf((x / period) + 0.5f));
}

static void Servo_VernierResetLate(void)
{
  s_vernier_late_hits = 0U;
  s_vernier_late_ms = Board_GetTickMs();
}

/**
 * @brief 上电定圈失败后的补定圈，在主循环里调用。
 *
 * 多圈位置由电机编码器逐 PWM 周期累加，一旦建立就是连续的，运行中不再读第二编码器。
 * 只有上电定圈失败时才在这里继续尝试：关桥且没有指令时每毫秒读一次游标，
 * 连续 SERVO_VERNIER_LATE_HITS 次解出同一个整圈修正量才采信，补的是整数圈，
 * 单圈角度和电角度不受影响。
 */
static void Servo_LateSeed(void)
{
  uint16_t raw1;
  uint16_t raw2;
  float raw_abs;
  float aligned_abs;
  float target;
  int32_t turns;
  uint32_t primask;
  const uint32_t now = Board_GetTickMs();

  if (s_abs_valid != 0U)
  {
    return;
  }
  if ((s_mode != SERVO_RUN) || (s_command_outputs_disabled == 0U) || (s_cmd_valid != 0U))
  {
    s_vernier_late_hits = 0U;
    return;
  }
  if ((now - s_vernier_late_ms) < 1U)
  {
    return;
  }
  s_vernier_late_ms = now;
  /** 读坏一帧不带任何信息，跳过即可；解不出圈号才说明当前位置不可信，要重新计数。 */
  if (Servo_SampleVernier(&raw1, &raw2) == 0U)
  {
    return;
  }
  if (Foc_VernierRawAbs(raw1, raw2, &raw_abs) == 0U)
  {
    s_vernier_late_hits = 0U;
    return;
  }
  aligned_abs = Foc_VernierAlignAbs(raw_abs, s_enc.direction);
  primask = Board_IrqLock();
  target = s_enc.mech_zero + Servo_WrapSym(aligned_abs - s_enc.mech_zero, SERVO_VERNIER_PERIOD_RAD);
  turns = (int32_t)floorf(((target - s_enc.mech_unwrapped) / FOC_TWO_PI) + 0.5f);
  Board_IrqUnlock(primask);
  if ((s_vernier_late_hits != 0U) && (turns != s_vernier_late_turns))
  {
    s_vernier_late_hits = 0U;
  }
  s_vernier_late_turns = turns;
  if (++s_vernier_late_hits < SERVO_VERNIER_LATE_HITS)
  {
    return;
  }
  s_vernier_late_hits = 0U;
  /** PWM 中断也在累加多圈角度，平移要关中断；期间若来了指令就放弃这次。 */
  primask = Board_IrqLock();
  if ((s_mode == SERVO_RUN) && (s_command_outputs_disabled != 0U) && (s_cmd_valid == 0U))
  {
    Foc_EncoderShiftTurns(&s_enc, turns);
    s_abs_valid = 1U;
    s_warning &= (uint8_t)~SERVO_WARN_ABS_UNKNOWN;
  }
  Board_IrqUnlock(primask);
}
#endif

static inline void Servo_ApplyDq(float d_v, float q_v, float elec, uint8_t slew, uint8_t early)
{
  Foc_Dq_t dq;
  Foc_AlphaBeta_t ab;
  float duty_a;
  float duty_b;
  float duty_c;

  dq.d = d_v;
  dq.q = q_v;
  Foc_LimitDQ(CFG_V_LIMIT, &dq.d, &dq.q);
  if (slew != 0U)
  {
    const float max_step = CFG_V_SLEW_PU_S * CFG_PWM_DT_S;
    s_d_out = Servo_Slew(s_d_out, dq.d, max_step);
    s_q_out = Servo_Slew(s_q_out, dq.q, max_step);
  }
  else
  {
    s_d_out = dq.d;
    s_q_out = dq.q;
  }
  dq.d = s_d_out;
  dq.q = s_q_out;
  ab = Foc_InversePark(dq, elec);
  Foc_SvpwmOffsetOptimized(ab.alpha, ab.beta, &duty_a, &duty_b, &duty_c);
#if CFG_CURRENT_EARLY_PWM
  if (early != 0U)
  {
    Pwm_ApplyDutyEarly(duty_a, duty_b, duty_c);
    return;
  }
#else
  (void)early;
#endif
  Pwm_ApplyDuty(duty_a, duty_b, duty_c);
}

void Servo_Init(void)
{
  Foc_EncoderConfig_t cfg;

  Foc_MathInit();
  cfg.pole_pairs = (uint8_t)CFG_POLE_PAIRS;
  cfg.direction = 1;
  cfg.electrical_offset_rad = 0.0f;
  cfg.vel_lpf_hz = CFG_VEL_LPF_HZ;
  cfg.sample_hz = (float)CFG_PWM_HZ;

  Foc_EncoderInit(&s_enc, &cfg);
  Foc_CurrentInit(&s_cur, CFG_MOTOR_R_OHM, CFG_MOTOR_L_H, CFG_CUR_BW_HZ,
                  CFG_VBUS_NOMINAL_V, CFG_I_LIMIT_A, CFG_V_LIMIT);
  s_mode = SERVO_IDLE;
  s_ctrl_mode = SERVO_CTRL_MOTION;
#if (BOARD_HAS_CURRENT_SENSE != 0U)
  s_inner_loop = CFG_INNER_LOOP;
#else
  s_inner_loop = CFG_INNER_LOOP_VOLTAGE;
#endif
  s_closed_loop_dir = 1;
  s_p_set = 0.0f;
  s_v_set = 0.0f;
  s_v_max = CFG_POS_VMAX_DEFAULT;
  s_kp = 0.0f;
  s_kd = 0.0f;
  s_t_ff = 0.0f;
  s_t_ref = 0.0f;
  s_vel_kp = Foc_Clamp(CFG_VEL_KP_DEFAULT, 0.0f, CFG_KP_VEL_MAX);
  s_vel_ki = Foc_Clamp(CFG_VEL_KI_DEFAULT, 0.0f, CFG_KI_VEL_MAX);
  s_pos_kp = Foc_Clamp(CFG_POS_KP_DEFAULT, 0.0f, CFG_KP_POS_MAX);
  s_pos_ki = Foc_Clamp(CFG_POS_KI_DEFAULT, 0.0f, CFG_KI_POS_MAX);
  s_pos_kd = Foc_Clamp(CFG_POS_KD_DEFAULT, 0.0f, CFG_KD_POS_MAX);
  s_vel_i = 0.0f;
  s_pos_i = 0.0f;
  s_cmd_valid = 0U;
  s_d_out = 0.0f;
  s_q_out = 0.0f;
  s_ia = 0.0f;
  s_ib = 0.0f;
  s_iq_ref = 0.0f;
  s_fault = 0U;
  s_current_missing = 0U;
  s_current_valid = 0U;
  s_encoder_missing = 0U;
  s_encoder_good = 0U;
  s_command_outputs_disabled = 1U;
  s_neutral_updates = 2U;
  s_enable_blank = 0U;
  s_oc_hits = 0U;
  s_uv_hits = 0U;
  s_warning = 0U;
  s_temp_valid = 0U;
  s_temp_c = 0.0f;
  s_temp_hot = 0U;
  s_temp_over = 0U;
  s_temp_ms = Board_GetTickMs();
#if (BOARD_HAS_ENCODER2 != 0U)
  s_abs_valid = 0U;
  Servo_VernierResetLate();
#endif
  Servo_ResetOuterGains();
}

/**
 * @brief 打开桥臂并屏蔽随后 CFG_PWM_ENABLE_BLANK 个采样。
 *
 * 开桥首个周期下管导通给自举电容充电，电流流经采样电阻，运放也需恢复，
 * 该期间的采样不代表相电流。量程内的大电流和 ADC 贴轨都只消耗屏蔽计数。
 * 屏蔽结束后贴轨仍立即判采样故障。调用方须保证 CCR 已是中性占空比，且在
 * 关中断或 PWM ISR 内调用。
 */
static void Servo_EnableOutputsBlanked(void)
{
  s_enable_blank = (uint8_t)CFG_PWM_ENABLE_BLANK;
  s_oc_hits = 0U;
  s_uv_hits = 0U;
  Pwm_EnableOutputs();
}

static void Servo_ApplyEncoderSample(uint8_t ok, uint16_t raw)
{
  if (ok != 0U)
  {
    const float mech = ((float)raw * FOC_TWO_PI) / (float)ENCODER_CPR;
    (void)Foc_EncoderUpdate(&s_enc, mech, CFG_PWM_DT_S);
  }
  else
  {
    Foc_EncoderPredict(&s_enc, CFG_PWM_DT_S);
  }
}

static uint8_t Servo_UpdateCurrent(float elec, float sample_offset_s, uint8_t run_pi, float iq_ref)
{
  float ia;
  float ib;
  const uint8_t result = Adc_ReadPhaseCurrents(&ia, &ib);
  const uint8_t active = (s_mode == SERVO_RUN) || (s_mode == SERVO_OPENLOOP) ||
                         (s_mode == SERVO_VOLTAGE);

  s_current_valid = 0U;
  if ((result != 1U) || !isfinite(ia) || !isfinite(ib))
  {
    /**
     * 开桥尖峰在 48 V 下会把运放打到轨。屏蔽拍内的贴轨和非有限值只丢弃，
     * 不闩锁采样故障。没有新转换仍按缺样计数。屏蔽结束后贴轨立即跳闸。
     */
    if ((s_enable_blank != 0U) && (result != 0U))
    {
      s_enable_blank--;
      s_current_missing = 0U;
      return 0U;
    }
    if (s_current_missing < 255U) { s_current_missing++; }
    if (active && ((result == 2U) ||
                   ((result == 1U) && (!isfinite(ia) || !isfinite(ib))) ||
                   (s_current_missing >= CFG_CURRENT_MISSING_MAX)))
    {
      Servo_Trip(SERVO_FAULT_CURRENT_SENSE);
    }
    /* Never integrate the previous sample a second time. */
    return 0U;
  }
  s_current_missing = 0U;
  if (s_enable_blank != 0U)
  {
    s_enable_blank--;
    return 0U;
  }
  s_ia = ia;
  s_ib = ib;
  s_current_valid = 1U;
  if (active && ((fabsf(ia) > CFG_I_TRIP_A) || (fabsf(ib) > CFG_I_TRIP_A) ||
                 (fabsf(ia + ib) > CFG_I_TRIP_A)))
  {
    if (++s_oc_hits >= (uint8_t)CFG_I_TRIP_CONFIRM)
    {
      Servo_Trip(SERVO_FAULT_OVERCURRENT);
    }
    /** 未确认的越限采样也不送入 PI。 */
    return 0U;
  }
  s_oc_hits = 0U;
  elec += Foc_EncoderGetVelocity(&s_enc) * (float)s_enc.pole_pairs * sample_offset_s;
  /**
   * 0 V 多半是这一拍规则组没转成，不是母线真的没了。沿用上一笔增益，
   * 只有连续读到低于 8 V 才跳欠压。
   */
  {
    const float vbus = Adc_GetVbusVolts();
    if (vbus >= CFG_VBUS_GAIN_MIN_V)
    {
      s_uv_hits = 0U;
      Foc_CurrentSetVbus(&s_cur, vbus);
    }
    else if ((run_pi != 0U) && (++s_uv_hits >= CFG_VBUS_UV_CONFIRM))
    {
      Servo_Trip(SERVO_FAULT_UNDERVOLTAGE);
      return 0U;
    }
  }
#if (CFG_COMMISSION != 0U)
  if (!Commission_Current(elec, ia, ib, iq_ref)) { return 0U; }
  if (Commission_DAxisStep())
    Foc_CurrentStep(&s_cur, s_ia, s_ib, elec, iq_ref, 0.0f, CFG_PWM_DT_S, run_pi);
  else
#endif
    Foc_CurrentStep(&s_cur, s_ia, s_ib, elec, 0.0f, iq_ref, CFG_PWM_DT_S, run_pi);
  return 1U;
}

static uint8_t Servo_UpdatePwmEncoder(void)
{
  uint16_t raw = 0U;
  const uint8_t ok = Encoder_ReadRawFast(&raw);
  s_enc1_ok = ok;
  if (ok != 0U)
  {
    s_enc1_raw = raw;
    s_encoder_missing = 0U;
    if (s_encoder_good < 255U) { s_encoder_good++; }
  }
  else
  {
    if (s_encoder_missing < 255U) { s_encoder_missing++; }
    s_encoder_good = 0U;
  }
  Servo_ApplyEncoderSample(ok, raw);
  if ((s_encoder_missing >= 4U) && (s_mode != SERVO_IDLE) && (s_mode != SERVO_FAULT))
  {
    Servo_Trip(SERVO_FAULT_ENCODER);
    return 0U;
  }
  return 1U;
}

static inline void Servo_RunCurrent(float elec, float signed_ref, uint8_t early)
{
  const float limit = CFG_DRIVE_I_LIMIT_A;
#if (CFG_COMMISSION != 0U)
  /* A zero-gain motion packet is a direct Iq/torque request, including
   * DJI group current. Apply the learned disturbance only to tracking. */
  const uint8_t tracking = s_ctrl_mode != SERVO_CTRL_MOTION || s_kp != 0.0f || s_kd != 0.0f;
  const float compensation = s_cmd_valid && tracking ? Commission_Cogging(s_enc1_raw) : 0.0f;
  const float target = Foc_Clamp(Commission_LimitIq(
      signed_ref * (CFG_I_LIMIT_A / CFG_V_LIMIT) + compensation,
      s_closed_loop_dir), -limit, limit);
#else
  const float target = Foc_Clamp(signed_ref * (CFG_I_LIMIT_A / CFG_V_LIMIT),
                                 -limit, limit);
#endif
  const uint8_t valid = s_cmd_valid;
  /* The current sample is near the peak. The encoder angle is from the
   * preceding valley on the ADC path, or the following valley on fallback. */
  const float sample_offset = (early != 0U) ? 0.5f * CFG_PWM_DT_S : -0.5f * CFG_PWM_DT_S;
  s_iq_ref = (valid != 0U) ? Servo_Slew(s_iq_ref, target, CFG_I_REF_SLEW_A_S * CFG_PWM_DT_S) : 0.0f;
  if (Servo_UpdateCurrent(elec, sample_offset, valid, s_iq_ref) != 0U)
  {
    if (early != 0U)
    {
      /* The early compares should latch at the next valley. */
      elec += Foc_EncoderGetVelocity(&s_enc) * (float)s_enc.pole_pairs * CFG_PWM_DT_S;
    }
    Servo_ApplyDq(s_cur.vd, s_cur.vq, elec, 0U, early);
  }
}

#if CFG_CURRENT_EARLY_PWM
uint8_t Servo_OnCurrentSampleIsr(void)
{
#if (CFG_COMMISSION != 0U)
  if (Commission_Active()) { return 0U; }
#endif
  if ((s_mode != SERVO_RUN) || (s_inner_loop != CFG_INNER_LOOP_CURRENT) ||
      (s_cmd_valid == 0U) || (s_command_outputs_disabled != 0U) || (s_enable_blank != 0U))
  {
    return 0U;
  }
  Servo_RunCurrent(Foc_EncoderGetElectrical(&s_enc), (float)s_closed_loop_dir * s_t_ref, 1U);
  return 1U;
}

void Servo_OnPwmEncoderIsr(void)
{
  (void)Servo_UpdatePwmEncoder();
}
#endif

void Servo_OnPwmIsr(void)
{
  const ServoMode_t mode = s_mode;
  const float sample_offset = -0.5f * CFG_PWM_DT_S;
  float elec;

  if (Servo_UpdatePwmEncoder() == 0U) { return; }
#if (CFG_COMMISSION != 0U)
  if (Commission_Pwm(s_enc1_raw, Foc_EncoderGetMechUnwrapped(&s_enc), Foc_EncoderGetVelocity(&s_enc)))
    return;
#endif
  if ((s_enable_blank != 0U) && (mode != SERVO_IDLE) && (mode != SERVO_FAULT))
  {
    (void)Servo_UpdateCurrent(Foc_EncoderGetElectrical(&s_enc), sample_offset, 0U, 0.0f);
    s_iq_ref = 0.0f;
    Pwm_ApplyDuty(0.5f, 0.5f, 0.5f);
    return;
  }

  if (mode == SERVO_OPENLOOP)
  {
    s_ol_elec = Foc_WrapAngle0To2Pi(s_ol_elec + (s_ol_rate * CFG_PWM_DT_S));
    elec = s_ol_elec;
    (void)Servo_UpdateCurrent(elec, sample_offset, 0U, 0.0f);
    if (s_mode == mode) { Servo_ApplyDq(s_ol_d, s_ol_q, elec, 1U, 0U); }
  }
  else if (mode == SERVO_VOLTAGE)
  {
    elec = Foc_EncoderGetElectrical(&s_enc);
    (void)Servo_UpdateCurrent(elec, sample_offset, 0U, 0.0f);
    if (s_mode == mode) { Servo_ApplyDq(s_volt_d, s_volt_q, elec, 1U, 0U); }
  }
  else if (mode == SERVO_RUN)
  {
    const float signed_ref = (float)s_closed_loop_dir * s_t_ref;
    elec = Foc_EncoderGetElectrical(&s_enc);
    if (s_command_outputs_disabled != 0U)
    {
      const uint8_t fresh = Servo_UpdateCurrent(elec, sample_offset, 0U, 0.0f);
      Servo_ApplyDq(0.0f, 0.0f, elec, 0U, 0U);
      s_iq_ref = 0.0f;
      /* CCR is preloaded: a command can arrive in the same RX batch as a
       * mode switch. Wait for neutral to cross two real update boundaries
       * before re-enabling MOE, so the old active duty cannot return. */
      if (s_neutral_updates != 0U) { s_neutral_updates--; }
      if ((fresh != 0U) && (s_fault == 0U) && (s_neutral_updates == 0U) &&
          (s_cmd_valid != 0U) && (s_enc1_ok != 0U))
      {
        if (Adc_GetVbusVolts() < CFG_VBUS_GAIN_MIN_V) { Servo_Trip(SERVO_FAULT_UNDERVOLTAGE); }
        else
        {
          Servo_EnableOutputsBlanked();
          s_command_outputs_disabled = 0U;
        }
      }
      return;
    }
    if (s_inner_loop == CFG_INNER_LOOP_CURRENT)
    {
      Servo_RunCurrent(elec, signed_ref, 0U);
    }
    else
    {
      (void)Servo_UpdateCurrent(elec, sample_offset, 0U, 0.0f);
      if (s_fault == 0U) { Servo_ApplyDq(0.0f, signed_ref, elec, 1U, 0U); }
    }
  }
  else
  {
    Servo_UpdateCurrent(Foc_EncoderGetElectrical(&s_enc), sample_offset, 0U, 0.0f);
  }
}

void Servo_OnCtrlIsr(void)
{
  float p_act;
  float v_act;
  const float dt = CFG_CTRL_DT_S;
  const float vlim = CFG_V_LIMIT;

#if CFG_MULTIAXIS
  if (MultiAxis_Busy())
  {
    p_act = Servo_MotorToOut(Foc_EncoderGetPosition(&s_enc));
    v_act = Servo_MotorToOut(Foc_EncoderGetVelocity(&s_enc));
    float ref, feedforward;
    if (MultiAxis_Control(p_act, v_act, &ref, &feedforward))
    {
      const float tracking = Servo_PiLimited(&s_pos_i, ref - p_act, s_pos_kp, s_pos_ki, dt, 30.0f);
      s_p_set = ref;
      s_v_set = Foc_Clamp(feedforward + tracking + s_pos_kd * (feedforward-v_act), -30.0f, 30.0f);
      s_t_ref = Servo_PiLimited(&s_vel_i, s_v_set-v_act, s_vel_kp, s_vel_ki, dt, vlim);
      s_cmd_valid = 1U;
    }
    return;
  }
#endif
  if (s_mode != SERVO_RUN)
  {
    return;
  }
  if (s_cmd_valid == 0U)
  {
    s_t_ref = 0.0f;
    return;
  }

  p_act = Servo_MotorToOut(Foc_EncoderGetPosition(&s_enc));
  v_act = Servo_MotorToOut(Foc_EncoderGetVelocity(&s_enc));

  if (s_ctrl_mode == SERVO_CTRL_MOTION)
  {
    const float t_ref = (s_kd * (s_v_set - v_act)) + (s_kp * (s_p_set - p_act)) + s_t_ff;
    s_t_ref = Foc_Clamp(t_ref, -vlim, vlim);
  }
  else if (s_ctrl_mode == SERVO_CTRL_VELOCITY)
  {
    s_t_ref = Servo_PiLimited(&s_vel_i, s_v_set - v_act, s_vel_kp, s_vel_ki, dt, vlim);
  }
  else
  {
    const float ep = s_p_set - p_act;
    const float d_term = -(s_pos_kd * v_act);
    const float vmax = s_v_max;
    const float v_lim = Servo_PosSpeedLimit(ep, vmax, CFG_POS_ACC_DEFAULT);
    const float v_prof = (ep >= 0.0f) ? v_lim : -v_lim;
    const float settle = CFG_POS_SETTLE_RAD;
    float v_pid;
    float v_ref;
    float inner_unsat;
    float pos_i_old;
    float w;

    pos_i_old = s_pos_i;
    if (vmax <= 0.0f)
    {
      s_pos_i = 0.0f;
      v_ref = 0.0f;
    }
    else
    {
      if (s_pos_ki <= 0.0f)
      {
        s_pos_i = 0.0f;
        v_pid = (s_pos_kp * ep) + d_term;
      }
      else
      {
        s_pos_i += s_pos_ki * ep * dt;
        s_pos_i = Foc_Clamp(s_pos_i, -vmax, vmax);
        v_pid = (s_pos_kp * ep) + s_pos_i + d_term;
      }
      v_pid = Foc_Clamp(v_pid, -vmax, vmax);
      if (fabsf(ep) >= settle)
      {
        w = 1.0f;
      }
      else if (settle <= 0.0f)
      {
        w = 0.0f;
      }
      else
      {
        w = fabsf(ep) / settle;
      }
      /* Far: follow vmax / sqrt(2 a |ep|). Near: hand off to PID. */
      v_ref = (w * v_prof) + ((1.0f - w) * v_pid);
      v_ref = Foc_Clamp(v_ref, -vmax, vmax);
    }

    inner_unsat = (s_vel_kp * (v_ref - v_act)) + s_vel_i + (s_vel_ki * (v_ref - v_act) * dt);
    s_t_ref = Servo_PiLimited(&s_vel_i, v_ref - v_act, s_vel_kp, s_vel_ki, dt, vlim);
    if ((inner_unsat > vlim) || (inner_unsat < -vlim))
    {
      s_pos_i = pos_i_old;
    }
  }
}

void Servo_SetMode(ServoMode_t mode)
{
  const uint32_t primask = Board_IrqLock();
  if ((s_fault != 0U) && (mode != SERVO_FAULT))
  {
    Board_IrqUnlock(primask);
    return;
  }
  if (mode != SERVO_RUN)
  {
    Servo_ClearLoopState();
    Foc_CurrentReset(&s_cur);
  }
  s_mode = mode;
  Board_IrqUnlock(primask);
}

ServoMode_t Servo_GetMode(void)
{
  return s_mode;
}

void Servo_SetOpenloop(float d_v, float q_v, float elec_rate_rad_s, float elec_angle_rad)
{
  const uint32_t primask = Board_IrqLock();
  if (s_fault != 0U) { Board_IrqUnlock(primask); return; }
  s_ol_d = d_v;
  s_ol_q = q_v;
  s_ol_rate = elec_rate_rad_s;
  s_ol_elec = Foc_WrapAngle0To2Pi(elec_angle_rad);
  /* Cali/openloop must not slew from the previous closed-loop voltage. */
  s_d_out = d_v;
  s_q_out = q_v;
  s_mode = SERVO_OPENLOOP;
  Board_IrqUnlock(primask);
}

void Servo_SetOpenloopRate(float elec_rate_rad_s)
{
  if (s_fault != 0U) { return; }
  s_ol_rate = elec_rate_rad_s;
  s_mode = SERVO_OPENLOOP;
}

float Servo_GetOpenloopElec(void)
{
  return s_ol_elec;
}

void Servo_SetVoltageCmd(float d_v, float q_v)
{
  const uint32_t primask = Board_IrqLock();
  if (s_fault != 0U) { Board_IrqUnlock(primask); return; }
  s_volt_d = d_v;
  s_volt_q = q_v;
  s_d_out = d_v;
  s_q_out = q_v;
  s_mode = SERVO_VOLTAGE;
  Board_IrqUnlock(primask);
}

void Servo_SetMotion(float p_set, float v_set, float t_ff)
{
  const uint32_t primask = Board_IrqLock();
  if (s_fault != 0U) { Board_IrqUnlock(primask); return; }
  s_p_set = p_set;
  s_v_set = Foc_Clamp(v_set, CFG_VEL_CMD_MIN, CFG_VEL_CMD_MAX);
  s_t_ff = Foc_Clamp(t_ff, -1.0f, 1.0f);
  s_cmd_valid = 1U;
  Board_IrqUnlock(primask);
}

void Servo_SetVelocityCmd(float v_set)
{
  const uint32_t primask = Board_IrqLock();
  if (s_fault != 0U) { Board_IrqUnlock(primask); return; }
  s_v_set = Foc_Clamp(v_set, CFG_VEL_CMD_MIN, CFG_VEL_CMD_MAX);
  s_cmd_valid = 1U;
  Board_IrqUnlock(primask);
}

void Servo_SetPositionCmd(float p_set, float v_max)
{
  const uint32_t primask = Board_IrqLock();
  if (s_fault != 0U) { Board_IrqUnlock(primask); return; }
  s_p_set = p_set;
  s_v_max = Foc_Clamp(v_max, 0.0f, CFG_VEL_CMD_MAX);
  s_cmd_valid = 1U;
  Board_IrqUnlock(primask);
}

void Servo_SetGains(float kp, float kd)
{
  const uint32_t primask = Board_IrqLock();
  s_kp = Foc_Clamp(kp, CFG_KP_MIN, CFG_KP_MAX);
  s_kd = Foc_Clamp(kd, CFG_KD_MIN, CFG_KD_MAX);
  Board_IrqUnlock(primask);
}

void Servo_GetGains(float *kp, float *kd)
{
  if (kp != 0)
  {
    *kp = s_kp;
  }
  if (kd != 0)
  {
    *kd = s_kd;
  }
}

void Servo_SetVelocityGains(float kp, float ki)
{
  const uint32_t primask = Board_IrqLock();
  s_vel_kp = Foc_Clamp(kp, 0.0f, CFG_KP_VEL_MAX);
  s_vel_ki = Foc_Clamp(ki, 0.0f, CFG_KI_VEL_MAX);
  if (s_vel_ki == 0.0f)
  {
    s_vel_i = 0.0f;
  }
  Board_IrqUnlock(primask);
}

void Servo_GetVelocityGains(float *kp, float *ki)
{
  if (kp != 0)
  {
    *kp = s_vel_kp;
  }
  if (ki != 0)
  {
    *ki = s_vel_ki;
  }
}

void Servo_SetPositionGains(float kp, float ki, float kd)
{
  const uint32_t primask = Board_IrqLock();
  s_pos_kp = Foc_Clamp(kp, 0.0f, CFG_KP_POS_MAX);
  s_pos_ki = Foc_Clamp(ki, 0.0f, CFG_KI_POS_MAX);
  s_pos_kd = Foc_Clamp(kd, 0.0f, CFG_KD_POS_MAX);
  if (s_pos_ki == 0.0f)
  {
    s_pos_i = 0.0f;
  }
  Board_IrqUnlock(primask);
}

void Servo_GetPositionGains(float *kp, float *ki, float *kd)
{
  if (kp != 0)
  {
    *kp = s_pos_kp;
  }
  if (ki != 0)
  {
    *ki = s_pos_ki;
  }
  if (kd != 0)
  {
    *kd = s_pos_kd;
  }
}

void Servo_SetCtrlMode(uint8_t mode)
{
  const uint32_t primask = Board_IrqLock();
  const ServoMode_t previous = Servo_PauseForFlash();
  s_ctrl_mode = mode;
  Servo_ResumeAfterFlash(previous);
  Board_IrqUnlock(primask);
}

uint8_t Servo_GetCtrlMode(void)
{
  return s_ctrl_mode;
}

void Servo_SetInnerLoop(uint8_t mode)
{
  const uint32_t primask = Board_IrqLock();
#if (BOARD_CURRENT_CONTROL_SUPPORTED == 0U)
  (void)mode;
  s_inner_loop = CFG_INNER_LOOP_VOLTAGE;
#else
  s_inner_loop = (mode == CFG_INNER_LOOP_CURRENT) ? CFG_INNER_LOOP_CURRENT
                                                  : CFG_INNER_LOOP_VOLTAGE;
#endif
  Servo_ClearLoopState();
  Servo_ResetOuterGains();
  Foc_CurrentReset(&s_cur);
  s_d_out = 0.0f;
  s_q_out = 0.0f;
  Board_IrqUnlock(primask);
}

uint8_t Servo_GetInnerLoop(void)
{
  return s_inner_loop;
}

void Servo_SetCurrentGains(float kp, float ki)
{
  const uint32_t lock = Board_IrqLock();
  s_cur.kp_v = kp;
  s_cur.ki_v = ki;
  Foc_CurrentReset(&s_cur);
  Board_IrqUnlock(lock);
}

void Servo_GetCurrentGains(float *kp, float *ki)
{
  const uint32_t lock = Board_IrqLock();
  *kp = s_cur.kp_v;
  *ki = s_cur.ki_v;
  Board_IrqUnlock(lock);
}

uint16_t Servo_GetFault(void)
{
  return s_fault;
}

/**
 * @brief 每 CFG_TEMP_POLL_MS 取一次板温，处理警告和过温故障。
 *
 * NTC 没焊在上电时已经排除。运行中读数超出范围（松脱、短路）只当作读不到：
 * 温度报无效并撤销警告，不跳闸，因为两种情况在读数上分不开，宁可漏报也不误报。
 */
static void Servo_MonitorTemperature(void)
{
  uint16_t raw = 0U;
  float t;
  uint32_t primask;
  const uint32_t now = Board_GetTickMs();

  if ((now - s_temp_ms) < CFG_TEMP_POLL_MS)
  {
    return;
  }
  s_temp_ms = now;
  if ((Adc_GetTempRaw(&raw) == 0U) || (Ntc_RawInRange(raw) == 0U))
  {
    s_temp_valid = 0U;
    s_temp_hot = 0U;
    s_temp_over = 0U;
    s_warning &= (uint8_t)~SERVO_WARN_TEMP;
    return;
  }
  t = Ntc_RawToCelsius(raw);
  s_temp_c = t;
  s_temp_valid = 1U;

  if (t >= CFG_TEMP_WARN_C)
  {
    if (s_temp_hot < CFG_TEMP_CONFIRM) { s_temp_hot++; }
    if (s_temp_hot >= CFG_TEMP_CONFIRM) { s_warning |= SERVO_WARN_TEMP; }
  }
  else
  {
    s_temp_hot = 0U;
    if (t < CFG_TEMP_WARN_CLEAR_C) { s_warning &= (uint8_t)~SERVO_WARN_TEMP; }
  }

  if (t >= CFG_TEMP_FAULT_C)
  {
    if (s_temp_over < CFG_TEMP_CONFIRM) { s_temp_over++; }
    if ((s_temp_over >= CFG_TEMP_CONFIRM) && ((s_fault & SERVO_FAULT_OVER_TEMP) == 0U))
    {
      primask = Board_IrqLock();
      Servo_Trip(SERVO_FAULT_OVER_TEMP);
      Board_IrqUnlock(primask);
    }
  }
  else
  {
    s_temp_over = 0U;
  }
}

void Servo_Service(void)
{
  Servo_MonitorTemperature();
#if (BOARD_HAS_ENCODER2 != 0U)
  Servo_LateSeed();
#endif
}

uint8_t Servo_GetTemperature(float *celsius_out)
{
  if (s_temp_valid == 0U)
  {
    return 0U;
  }
  *celsius_out = s_temp_c;
  return 1U;
}

uint8_t Servo_ClearFault(void)
{
  const uint32_t primask = Board_IrqLock();
  const float vbus = Adc_GetVbusVolts();
  if ((Adc_CurrentSenseReady() == 0U) || (s_current_valid == 0U) ||
      (s_encoder_missing != 0U) || !isfinite(vbus) || (vbus < CFG_VBUS_GAIN_MIN_V) ||
      (fabsf(s_ia) > 1.0f) || (fabsf(s_ib) > 1.0f) || (fabsf(s_ia + s_ib) > 1.0f) ||
      (((s_fault & SERVO_FAULT_OVER_TEMP) != 0U) && (s_temp_valid != 0U) &&
       (s_temp_c >= CFG_TEMP_WARN_CLEAR_C)))
  {
    Board_IrqUnlock(primask);
    return 0U;
  }
  s_fault = 0U;
  s_temp_over = 0U;
  s_mode = SERVO_IDLE;
  Servo_ClearLoopState();
  Foc_CurrentReset(&s_cur);
  Pwm_DisableOutputs();
  Pwm_ApplyDuty(0.5f, 0.5f, 0.5f);
  s_command_outputs_disabled = 1U;
  s_neutral_updates = 2U;
  s_enable_blank = 0U;
  s_oc_hits = 0U;
  s_uv_hits = 0U;
  Board_IrqUnlock(primask);
  return 1U;
}

uint8_t Servo_EnableCalibrationOutputs(void)
{
  uint32_t primask;
  Adc_RefreshVbus();
  primask = Board_IrqLock();
  if ((s_fault != 0U) || (Adc_CurrentSenseReady() == 0U) ||
      !(Adc_GetVbusVolts() >= CFG_VBUS_GAIN_MIN_V))
  {
    Board_IrqUnlock(primask);
    return 0U;
  }
  Pwm_ApplyDuty(0.5f, 0.5f, 0.5f);
  Servo_EnableOutputsBlanked();
  Board_IrqUnlock(primask);
  return 1U;
}

ServoMode_t Servo_PauseForFlash(void)
{
  const uint32_t primask = Board_IrqLock();
  const ServoMode_t previous = s_mode;
  s_mode = (s_fault != 0U || previous == SERVO_FAULT) ? SERVO_FAULT : SERVO_IDLE;
  Pwm_DisableOutputs();
  s_command_outputs_disabled = 1U;
  s_neutral_updates = 2U;
  s_enable_blank = 0U;
  s_oc_hits = 0U;
  s_uv_hits = 0U;
  Servo_ClearLoopState();
  Foc_CurrentReset(&s_cur);
  s_d_out = 0.0f;
  s_q_out = 0.0f;
  Pwm_ApplyDuty(0.5f, 0.5f, 0.5f);
  Board_IrqUnlock(primask);
  return previous;
}

void Servo_ResumeAfterFlash(ServoMode_t previous)
{
  const uint32_t primask = Board_IrqLock();
  Servo_ClearLoopState();
  Foc_CurrentReset(&s_cur);
  s_current_missing = 0U;
  if ((s_fault == 0U) && (previous == SERVO_RUN)) { s_mode = SERVO_RUN; }
  /* Bridge stays disabled until a fresh command. Faults remain latched. */
  Board_IrqUnlock(primask);
}

void Servo_SetZero(void)
{
  const uint32_t primask = Board_IrqLock();
  Foc_EncoderSetZero(&s_enc);
  s_p_set = 0.0f;
  s_vel_i = 0.0f;
  s_pos_i = 0.0f;
  Board_IrqUnlock(primask);
}

uint8_t Servo_EncoderStable(void)
{
  return (s_encoder_good >= (uint8_t)CFG_ENC_STABLE_SAMPLES) ? 1U : 0U;
}

uint8_t Servo_SeedMultiTurn(float zero_motor_rad)
{
  uint16_t raw1 = 0U;
  float raw_single;
  uint32_t primask;
#if (BOARD_HAS_ENCODER2 == 0U)
  if (Encoder_ReadFrame(&raw1, 0) == 0U)
  {
    return 0U;
  }
  raw_single = ((float)raw1 * FOC_TWO_PI) / (float)ENCODER_CPR;

  primask = Board_IrqLock();
  Foc_EncoderSeedSingleTurn(&s_enc, raw_single, zero_motor_rad);
#else
  uint16_t raw2 = 0U;
  float raw_abs;
  float aligned_abs = 0.0f;
  float last_abs = 0.0f;
  uint8_t hits = 0U;
  uint8_t attempt;

  /**
   * 单次解码可能因齿隙差一整圈，也可能赶上第二编码器还没上电完成。
   * 连续多次解码落在同一处才采信。
   */
  for (attempt = 0U; (attempt < SERVO_VERNIER_SEED_TRIES) && (hits < SERVO_VERNIER_SEED_HITS); attempt++)
  {
    if (attempt != 0U)
    {
      Board_DelayMs(1U);
    }
    if ((Encoder_ReadFrame(&raw1, 0) == 0U) || (Encoder2_ReadFrame(&raw2, 0) == 0U) ||
        (Foc_VernierRawAbs(raw1, raw2, &raw_abs) == 0U))
    {
      hits = 0U;
      continue;
    }
    aligned_abs = Foc_VernierAlignAbs(raw_abs, s_enc.direction);
    if ((hits != 0U) &&
        (fabsf(Servo_WrapSym(aligned_abs - last_abs, SERVO_VERNIER_PERIOD_RAD)) > FOC_PI))
    {
      hits = 0U;
    }
    last_abs = aligned_abs;
    hits++;
  }
  if (hits < SERVO_VERNIER_SEED_HITS)
  {
    return 0U;
  }
  raw_single = ((float)raw1 * FOC_TWO_PI) / (float)ENCODER_CPR;
  /**
   * 游标每 N2 圈重复一次。在等价角度里取离零点最近的一个，
   * 这样零点两侧各有半个周期的行程，不会在周期接缝处差出一整个周期。
   */
  aligned_abs = zero_motor_rad +
                Servo_WrapSym(aligned_abs - zero_motor_rad, SERVO_VERNIER_PERIOD_RAD);

  primask = Board_IrqLock();
  Foc_EncoderSeedAbs(&s_enc, raw_single, aligned_abs);
  Foc_EncoderSetZeroAbs(&s_enc, zero_motor_rad);
  s_abs_valid = 1U;
  s_warning &= (uint8_t)~SERVO_WARN_ABS_UNKNOWN;
  Servo_VernierResetLate();
#endif
  s_enc1_raw = raw1;
  s_enc1_ok = 1U;
  Board_IrqUnlock(primask);
  return 1U;
}

void Servo_SeedFallback(float raw_mech_rad, float zero_motor_rad)
{
  const uint32_t primask = Board_IrqLock();

  Foc_EncoderSeedSingleTurn(&s_enc, raw_mech_rad, zero_motor_rad);
#if (BOARD_HAS_ENCODER2 != 0U)
  s_abs_valid = 0U;
  s_warning |= SERVO_WARN_ABS_UNKNOWN;
  Servo_VernierResetLate();
#endif
  Board_IrqUnlock(primask);
}

uint8_t Servo_SampleVernier(uint16_t *raw1_out, uint16_t *raw2_out)
{
#if (BOARD_HAS_ENCODER2 == 0U)
  (void)raw1_out;
  (void)raw2_out;
  return 0U;
#else
  uint16_t raw2 = 0U;
  uint8_t ok;
  const uint32_t primask = Board_IrqLock();

  ok = ((s_enc1_ok != 0U) && (Encoder2_ReadFrame(&raw2, 0) != 0U)) ? 1U : 0U;
  *raw1_out = s_enc1_raw;
  *raw2_out = raw2;
  Board_IrqUnlock(primask);
  return ok;
#endif
}

uint8_t Servo_GetWarning(void)
{
  return s_warning;
}

float Servo_GetZeroMotor(void)
{
  return s_enc.mech_zero;
}

void Servo_SetClosedLoopDir(int8_t dir)
{
  s_closed_loop_dir = (dir < 0) ? -1 : 1;
}

void Servo_SetEncoderAlignment(int8_t encoder_dir, float electrical_offset_rad)
{
  Foc_EncoderSetAlignment(&s_enc, encoder_dir, electrical_offset_rad);
}

void Servo_SetPolePairs(uint8_t pole_pairs)
{
  Foc_EncoderSetPolePairs(&s_enc, pole_pairs);
}

void Servo_HoldPosition(void)
{
  const uint32_t primask = Board_IrqLock();
  s_p_set = Servo_MotorToOut(Foc_EncoderGetPosition(&s_enc));
  s_v_set = 0.0f;
  s_v_max = CFG_POS_VMAX_DEFAULT;
  s_t_ff = 0.0f;
  s_t_ref = 0.0f;
  s_vel_i = 0.0f;
  s_pos_i = 0.0f;
  if ((s_ctrl_mode == SERVO_CTRL_MOTION) || (s_ctrl_mode == SERVO_CTRL_POSITION))
  {
    s_cmd_valid = 1U;
  }
  else
  {
    s_cmd_valid = 0U;
  }
  Board_IrqUnlock(primask);
}

void Servo_GetTelemetry(ServoTelemetry_t *out)
{
  uint32_t primask;
  if (out == 0)
  {
    return;
  }
  primask = Board_IrqLock();
  out->p_set = s_p_set;
  out->v_set = s_v_set;
  out->kp = s_kp;
  out->kd = s_kd;
  out->t_ff = s_t_ff;
  out->t_ref = s_t_ref;
  out->p_act = Servo_MotorToOut(Foc_EncoderGetPosition(&s_enc));
  out->v_act = Servo_MotorToOut(Foc_EncoderGetVelocity(&s_enc));
  out->elec = Foc_EncoderGetElectrical(&s_enc);
  out->ia = s_cur.ia;
  out->ib = s_cur.ib;
  out->id = s_cur.id;
  out->iq = s_cur.iq;
  out->iq_ref = s_iq_ref;
  out->torque_nm = Cfg_MotorTorqueNm(s_cur.iq);
  out->vq = s_q_out;
  out->closed_loop_dir = s_closed_loop_dir;
  out->ctrl_mode = s_ctrl_mode;
  out->inner_loop = s_inner_loop;
  Board_IrqUnlock(primask);
}

Foc_Encoder_t *Servo_GetEncoder(void)
{
  return &s_enc;
}
