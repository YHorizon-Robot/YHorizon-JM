#include "commission.h"

#include "adc.h"
#include "board.h"
#include "cali.h"
#include "can.h"
#include "config.h"
#include "encoder.h"
#include "debug.h"
#include "foc_math.h"
#include "foc_vernier.h"
#include "nv_flash.h"
#include "pwm.h"
#include "servo.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

enum { INFO = 0x30, START, STATUS, SAMPLE, STOP, READ, WRITE, PROFILE };
enum { ENC = 1, DIR, RES, LD, LQ, CURRENT, VELOCITY, POSITION, COG, COG_CHECK };
enum { IDLE, PREPARE, RUN, DONE, FAILED };
enum { INVALID = 1, BUSY, LIMIT, NO_CALI, FAULT, TIMEOUT, DATA, STORAGE,
       TRAVEL, SPEED, CURRENT_LIMIT, SUPPLY };
enum { FLAGS, RS, LD_H, LQ_H, CURRENT_KP, CURRENT_KI, ENCODER, OFFSET,
       VEL_KP, VEL_KI, POS_KP, POS_KI, POS_KD, TABLE = 16 };
enum { HAS_RL = 1, HAS_CURRENT = 2, HAS_PID = 4, HAS_ENC = 8, HAS_AUX = 16, HAS_COG = 32 };
#define COG_POINTS 256U
#define WORDS (TABLE + COG_POINTS / 2U)
#define SAMPLES 2048U
#define PWM_MS (CFG_PWM_HZ / 1000U)
#define PROFILE_MAGIC 0x59434D31UL
#define TRIAL_I_MAX 1.0f

typedef struct
{
  uint32_t magic;
  uint32_t generation;
  uint32_t words[WORDS];
  uint32_t reserved;
  uint32_t crc;
} Record;

typedef struct { uint16_t a, b, c; } Sample;

static Record s_record;
static uint8_t s_slot;
static uint32_t s_stage[WORDS];
static uint8_t s_stage_ready;
static uint8_t s_drive_held;
static uint32_t s_drive_seen;
static Sample s_samples[SAMPLES];

static struct
{
  volatile uint8_t state;
  volatile uint8_t error;
  uint8_t kind;
  int8_t variant;
  int16_t arg;
  uint16_t token;
  uint8_t was_calibrated;
  uint8_t old_mode;
  CaliNvData_t prior_alignment;
  float prior_control[7];
  uint32_t preparation_ms;
  uint32_t ticks;
  uint32_t end;
  uint16_t speed_hits;
  uint16_t last_angle_slot;
  volatile uint32_t lease;
  volatile uint16_t count;
  float origin;
  float fixed_elec;
  float vbus;
  float delayed_v;
  float sampled_v;
  float d_pu;
  float q_pu;
  uint16_t raw;
  uint8_t restored;
} s;

static uint32_t Read32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t Read16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void Put16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void Put32(uint8_t *p, uint32_t value)
{
  Put16(p, (uint16_t)value);
  Put16(p + 2, (uint16_t)(value >> 16));
}

static uint32_t Bits(float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static float Value(uint32_t bits)
{
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t Crc(const void *ptr, size_t size)
{
  const uint8_t *p = ptr;
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < size; ++i)
  {
    crc ^= p[i];
    for (uint8_t b = 0; b < 8U; ++b)
      crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
  }
  return ~crc;
}

static int16_t Milli(float x)
{
  return (int16_t)Foc_Clamp(roundf(1000.0f * x), -32768.0f, 32767.0f);
}

static int16_t CogAt(const uint32_t *words, unsigned i)
{
  return (int16_t)(words[TABLE + i / 2U] >> ((i & 1U) * 16U));
}

static uint8_t InRange(float x, float min, float max)
{
  return (uint8_t)(isfinite(x) && x >= min && x <= max);
}

static uint8_t Valid(const uint32_t *w)
{
  const uint32_t flags = w[FLAGS];
  if ((flags & ~63UL) != 0U || ((flags & HAS_AUX) && !(flags & HAS_ENC))) { return 0U; }
  if ((flags & HAS_RL) &&
      (!InRange(Value(w[RS]), 0.03f, 40.0f) ||
       !InRange(Value(w[LD_H]), 0.000005f, 0.1f) ||
       !InRange(Value(w[LQ_H]), 0.000005f, 0.1f))) { return 0U; }
  if ((flags & HAS_CURRENT) &&
      (!InRange(Value(w[CURRENT_KP]), 0.01f, 22.0f) ||
       !InRange(Value(w[CURRENT_KI]), 1.0f, 60000.0f))) { return 0U; }
  if (flags & HAS_ENC)
  {
    const uint8_t pp = (uint8_t)w[ENCODER];
    const int8_t dir = (int8_t)(w[ENCODER] >> 8);
    const int8_t torque = (int8_t)(w[ENCODER] >> 16);
    if (pp < CFG_CALI_PP_MIN || pp > CFG_CALI_PP_MAX ||
        (dir != 1 && dir != -1) || (torque != 1 && torque != -1) ||
        !InRange(Value(w[OFFSET]), 0.0f, FOC_TWO_PI)) { return 0U; }
  }
  if ((flags & HAS_PID) &&
      (!InRange(Value(w[VEL_KP]), 0.0f, CFG_KP_VEL_MAX) ||
       !InRange(Value(w[VEL_KI]), 0.0f, CFG_KI_VEL_MAX) ||
       !InRange(Value(w[POS_KP]), 0.0f, CFG_KP_POS_MAX) ||
       !InRange(Value(w[POS_KI]), 0.0f, CFG_KI_POS_MAX) ||
       !InRange(Value(w[POS_KD]), 0.0f, CFG_KD_POS_MAX))) { return 0U; }
  if (flags & HAS_COG)
    for (unsigned i = 0; i < COG_POINTS; ++i)
      if (CogAt(w, i) < -400 || CogAt(w, i) > 400) { return 0U; }
  return 1U;
}

void Commission_Init(void)
{
  memset(&s_record, 0, sizeof(s_record));
  memset(&s, 0, sizeof(s));
  s_slot = 0U;
  s_stage_ready = 0U;
  s_drive_held = 0U;
  for (uint8_t slot = 0; slot < 2U; ++slot)
  {
    Record candidate;
    NvFlash_Read(BOARD_NV_PAGE_ADDR - (2U - slot) * 2048U, &candidate, sizeof(candidate));
    if (candidate.magic == PROFILE_MAGIC && candidate.reserved == 0U &&
        candidate.crc == Crc(&candidate, offsetof(Record, crc)) && Valid(candidate.words) &&
        (s_record.magic != PROFILE_MAGIC ||
         (int32_t)(candidate.generation - s_record.generation) > 0))
    {
      s_record = candidate;
      s_slot = slot;
    }
  }
  Dbg_Printf("JM1 saved profile gen=%u flags=0x%x\r\n",
             (unsigned)s_record.generation, (unsigned)s_record.words[FLAGS]);
}

static uint8_t Save(const uint32_t *words)
{
  static Record next;
  const uint8_t slot = s_slot ^ 1U;
  next.magic = PROFILE_MAGIC;
  next.generation = s_record.generation + 1U;
  memcpy(next.words, words, sizeof(next.words));
  next.reserved = 0U;
  next.crc = Crc(&next, offsetof(Record, crc));
  Can_StopForFlash();
  const uint8_t written = NvFlash_WriteCommissionPage(slot, &next, sizeof(next));
  Can_Restart();
  Adc_RefreshVbus();
  if (!written) { return 0U; }
  /* Read back the new slot before selecting it. The prior slot stays intact. */
  const uint32_t expected = next.crc;
  NvFlash_Read(BOARD_NV_PAGE_ADDR - (2U - slot) * 2048U, &next, sizeof(next));
  if (next.magic != PROFILE_MAGIC || next.crc != expected ||
      Crc(&next, offsetof(Record, crc)) != expected) { return 0U; }
  s_record = next;
  s_slot = slot;
  return 1U;
}

static void DecodeAlignment(const uint32_t *w, CaliNvData_t *nv)
{
  nv->pole_pairs = (uint8_t)w[ENCODER];
  nv->encoder_dir = (int8_t)(w[ENCODER] >> 8);
  nv->closed_loop_dir = (int8_t)(w[ENCODER] >> 16);
  nv->enc2_offset_raw = (int8_t)(w[ENCODER] >> 24);
  nv->enc2_offset_valid = (w[FLAGS] & HAS_AUX) != 0U;
  nv->electrical_offset_rad = Value(w[OFFSET]);
}

uint8_t Commission_MergeAlignment(CaliNvData_t *nv, uint8_t legacy_valid)
{
  if (!(s_record.words[FLAGS] & HAS_ENC)) { return legacy_valid; }
  if (!legacy_valid && !CaliNv_LoadMetadata(nv)) { memset(nv, 0, sizeof(*nv)); }
  const int8_t stored_dir = nv->encoder_dir < 0 ? -1 : 1;
  DecodeAlignment(s_record.words, nv);
  nv->mt_zero_rad *= (float)(stored_dir * nv->encoder_dir);
  if (stored_dir != nv->encoder_dir) { nv->user_gains_valid = 0U; }
  return 1U;
}

float Commission_LegacyZero(float aligned_zero)
{
  if (s_record.words[FLAGS] & HAS_ENC)
  {
    CaliNvData_t nv;
    const int8_t dir = CaliNv_LoadMetadata(&nv) && nv.encoder_dir < 0 ? -1 : 1;
    const int8_t host_dir = (int8_t)(s_record.words[ENCODER] >> 8);
    return aligned_zero * (float)(dir * host_dir);
  }
  return aligned_zero;
}

static void ApplyControl(const uint32_t *w)
{
  if (w[FLAGS] & HAS_CURRENT)
    Servo_SetCurrentGains(Value(w[CURRENT_KP]), Value(w[CURRENT_KI]));
  if ((w[FLAGS] & HAS_PID) && Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT)
  {
    Servo_SetVelocityGains(Value(w[VEL_KP]), Value(w[VEL_KI]));
    Servo_SetPositionGains(Value(w[POS_KP]), Value(w[POS_KI]), Value(w[POS_KD]));
  }
}

void Commission_ApplyControl(void) { ApplyControl(s_record.words); }

float Commission_PhaseResistance(void)
{
  return s_record.words[FLAGS] & HAS_RL ? Value(s_record.words[RS]) : CFG_MOTOR_R_OHM;
}

float Commission_CurrentBandwidth(void)
{
  const uint32_t *w = s_record.words;
  return (w[FLAGS] & (HAS_RL | HAS_CURRENT)) == (HAS_RL | HAS_CURRENT) ?
      Value(w[CURRENT_KI]) / (Value(w[RS]) * FOC_TWO_PI) : CFG_CUR_BW_HZ;
}

uint8_t Commission_OwnsGains(void)
{
  return (s_record.words[FLAGS] & HAS_PID) && Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT;
}

static void ReadControl(float values[7])
{
  Servo_GetCurrentGains(&values[0], &values[1]);
  Servo_GetVelocityGains(&values[2], &values[3]);
  Servo_GetPositionGains(&values[4], &values[5], &values[6]);
}

static void StoreControl(uint32_t *w)
{
  float values[7];
  ReadControl(values);
  w[CURRENT_KP] = Bits(values[0]);
  w[CURRENT_KI] = Bits(values[1]);
  /* The persisted outer gains belong to the current-loop control scale. */
  if (!(w[FLAGS] & HAS_PID) || Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT)
    for (unsigned i = 0; i < 5U; ++i) { w[VEL_KP + i] = Bits(values[i + 2U]); }
}

uint8_t Commission_SaveGains(void)
{
  uint32_t w[WORDS];
  memcpy(w, s_record.words, sizeof(w));
  float v[7];
  ReadControl(v);
  for (unsigned i = 0; i < 5U; ++i) { w[VEL_KP + i] = Bits(v[i + 2U]); }
  return Save(w);
}

static void ReadAlignment(CaliNvData_t *nv)
{
  const Foc_Encoder_t *enc = Servo_GetEncoder();
  ServoTelemetry_t tel;
  Servo_GetTelemetry(&tel);
  nv->pole_pairs = enc->pole_pairs;
  nv->encoder_dir = enc->direction;
  nv->electrical_offset_rad = enc->electrical_offset_rad;
  nv->closed_loop_dir = tel.closed_loop_dir;
  nv->enc2_offset_raw = (int8_t)roundf(Foc_VernierGetOffset() * 256.0f);
  nv->mt_zero_rad = Servo_GetZeroMotor();
}

static void StageFromLive(uint8_t calibrated)
{
  memcpy(s_stage, s_record.words, sizeof(s_stage));
  StoreControl(s_stage);
  if (calibrated && !(s_stage[FLAGS] & HAS_ENC))
  {
    CaliNvData_t nv = {0};
    const uint8_t have_nv = CaliNv_Load(&nv);
    ReadAlignment(&nv);
    s_stage[FLAGS] |= HAS_ENC;
    if (have_nv && nv.enc2_offset_valid) { s_stage[FLAGS] |= HAS_AUX; }
    s_stage[ENCODER] = (uint8_t)nv.pole_pairs | ((uint32_t)(uint8_t)nv.encoder_dir << 8) |
        ((uint32_t)(uint8_t)nv.closed_loop_dir << 16) | ((uint32_t)(uint8_t)nv.enc2_offset_raw << 24);
    s_stage[OFFSET] = Bits(nv.electrical_offset_rad);
  }
  s_stage_ready = 1U;
}

uint8_t Commission_Active(void)
{
  return s.state != IDLE && !s.restored;
}

static uint8_t DriveHeld(void)
{
  return s_drive_held && (uint32_t)(Board_GetTickMs() - s_drive_seen) < 2000U;
}

uint8_t Commission_BlocksDrive(void)
{
  return Commission_Active() || DriveHeld();
}

static void End(uint8_t error)
{
  (void)Servo_PauseForFlash();
  s.error = error;
  s.state = error == 0U ? DONE : FAILED;
}

void Commission_Abort(void)
{
  const uint32_t lock = Board_IrqLock();
  s_drive_held = 0U;
  if (s.state == PREPARE || s.state == RUN) { End(TIMEOUT); }
  Board_IrqUnlock(lock);
}

static void Push(uint16_t a, uint16_t b, uint16_t c)
{
  if (s.count >= SAMPLES) { End(DATA); return; }
  s_samples[s.count++] = (Sample){a, b, c};
}

static void ApplyAlignment(const CaliNvData_t *a)
{
  const uint32_t lock = Board_IrqLock();
  Foc_Encoder_t *enc = Servo_GetEncoder();
  const float raw = Foc_EncoderGetLastRaw(enc);
  const float absolute = Foc_EncoderGetMechUnwrapped(enc) * (float)(enc->direction * a->encoder_dir);
  Servo_SetPolePairs(a->pole_pairs);
  Servo_SetEncoderAlignment(a->encoder_dir, a->electrical_offset_rad);
  Foc_EncoderSeedAbs(enc, raw, absolute);
  Servo_SetClosedLoopDir(a->closed_loop_dir);
  Foc_VernierSetOffset((float)a->enc2_offset_raw / 256.0f);
  Board_IrqUnlock(lock);
}

static void Restore(void)
{
  if (s.kind == DIR)
  {
    ApplyAlignment(&s.prior_alignment);
    if (!Servo_SeedMultiTurn(s.prior_alignment.mt_zero_rad))
      Servo_SeedFallback(Foc_EncoderGetLastRaw(Servo_GetEncoder()), s.prior_alignment.mt_zero_rad);
  }
  if (s.kind >= CURRENT)
  {
    float *p = s.prior_control;
    Servo_SetCurrentGains(p[0], p[1]);
    Servo_SetVelocityGains(p[2], p[3]);
    Servo_SetPositionGains(p[4], p[5], p[6]);
    Servo_SetCtrlMode(s.old_mode);
  }
  if (s.was_calibrated) { Servo_ResumeAfterFlash(SERVO_RUN); }
  s.restored = 1U;
}

static uint8_t Arm(void)
{
  const Foc_Encoder_t *enc = Servo_GetEncoder();
  s.origin = Foc_EncoderGetMechUnwrapped(enc);
  s.fixed_elec = Foc_EncoderGetElectrical(enc);
  s.ticks = 0U;
  s.speed_hits = 0U;
  s.last_angle_slot = 0U;
  s.d_pu = 0.0f;
  s.q_pu = 0.0f;
  s.delayed_v = 0.0f;
  s.sampled_v = 0.0f;
  s.vbus = Adc_GetVbusVolts();

  if (!Servo_EncoderStable() || !Adc_CurrentSenseReady() ||
      !InRange(s.vbus, CFG_VBUS_GAIN_MIN_V, 60.0f)) { return FAULT; }
  if (s.kind <= LQ)
  {
    if (!Servo_EnableCalibrationOutputs()) { return FAULT; }
    if (s.kind == ENC)
    {
      s.fixed_elec = s.was_calibrated ? Foc_EncoderGetElectrical(enc) : 0.0f;
      Servo_SetOpenloop(0.0f, 0.0f, 0.0f, s.fixed_elec);
    }
    else if (s.kind == RES) { Servo_SetOpenloop(0.0f, 0.0f, 0.0f, s.fixed_elec); }
    else { Servo_SetVoltageCmd(0.0f, 0.0f); }
  }
  else
  {
    ApplyControl(s_stage);
    Servo_SetCtrlMode(s.kind == CURRENT ? SERVO_CTRL_MOTION :
                      s.kind == POSITION ? SERVO_CTRL_POSITION : SERVO_CTRL_VELOCITY);
    Servo_ResumeAfterFlash(SERVO_RUN);
    if (s.kind == POSITION)
    {
      ServoTelemetry_t tel;
      Servo_GetTelemetry(&tel);
      Servo_SetPositionCmd(tel.p_act + (float)s.arg * 0.001f, 0.45f);
    }
    else if (s.kind >= VELOCITY) { Servo_SetVelocityCmd((float)s.arg * 0.001f); }
    else
    {
      /* A zero target establishes current samples before the test step. */
      Servo_SetMotion(0.0f, 0.0f, 0.0f);
    }
  }
  return 0U;
}

void Commission_Service(void)
{
  if ((s.state == PREPARE || s.state == RUN) && Servo_GetFault())
  {
    const uint32_t lock = Board_IrqLock();
    if (s.state == PREPARE || s.state == RUN) { End(FAULT); }
    Board_IrqUnlock(lock);
  }
  if (s.state == PREPARE && Board_GetTickMs() - s.preparation_ms >= 3U)
  {
    if (s.kind == DIR)
    {
      CaliNvData_t a = {0};
      DecodeAlignment(s_stage, &a);
      ApplyAlignment(&a);
    }
    const uint32_t lock = Board_IrqLock();
    const uint8_t error = Arm();
    if (error != 0U) { End(error); }
    else { s.state = RUN; }
    Board_IrqUnlock(lock);
  }
  if (s.state == RUN && s.kind == ENC)
  {
    /* The second SPI encoder is read outside the PWM ISR. Index gaps are rejected
       by the upper-level angular travel and phase-consistency tests. */
    const uint16_t expected = (uint16_t)(s.ticks / (10U * PWM_MS));
    if (expected != s.last_angle_slot && s.ticks > 650U * PWM_MS)
    {
      s.last_angle_slot = expected;
      uint16_t raw1 = 0U, raw2 = 0U;
#if BOARD_HAS_ENCODER2 != 0U
      const uint8_t ok = Servo_SampleVernier(&raw1, &raw2);
#else
      const uint8_t ok = 1U;
      raw1 = s.raw;
#endif
      const uint32_t lock = Board_IrqLock();
      if (s.state == RUN && ok)
      {
        const uint16_t elec = (uint16_t)((uint32_t)(Servo_GetOpenloopElec() * (65536.0f / FOC_TWO_PI)));
        Push(raw1, raw2, elec);
      }
      Board_IrqUnlock(lock);
    }
  }
  if ((s.state == DONE || s.state == FAILED) && !s.restored) { Restore(); }
}

static float LimitTravel(uint8_t kind)
{
  if (kind == ENC) { return 9.5f; }
  if (kind == DIR) { return 3.5f; }
  if (kind >= RES && kind <= LQ) { return 0.14f; }
  if (kind == CURRENT) { return 5.0f; }
  if (kind >= COG) { return 26.0f; }
  return Foc_Clamp(CFG_GEAR_RATIO * 0.85f, 3.0f, 26.0f);
}

uint8_t Commission_Pwm(uint16_t raw, float motor_rad, float motor_speed)
{
  s.raw = raw;
  if (s.state != RUN) { return 0U; }
  s.sampled_v = s.delayed_v;
  s.delayed_v = (s.kind == LQ ? s.q_pu : s.d_pu) * s.vbus;
  if (s.lease == 0U || --s.lease == 0U) { End(TIMEOUT); return 1U; }
  if (Servo_GetFault() != 0U || !Servo_EncoderStable()) { End(FAULT); return 1U; }
  if (fabsf(motor_rad - s.origin) > LimitTravel(s.kind)) { End(TRAVEL); return 1U; }
  const float speed_limit = s.kind >= COG ? 9.0f :
      s.kind == ENC || s.kind == DIR ? 20.0f :
      s.kind >= VELOCITY ? 2.0f * CFG_GEAR_RATIO : 16.0f;
  if (fabsf(motor_speed) > speed_limit)
  {
    if (++s.speed_hits >= PWM_MS * 3U) { End(SPEED); return 1U; }
  }
  else { s.speed_hits = 0U; }
  if (++s.ticks >= s.end) { End(0); return 1U; }

  if (s.kind == ENC)
  {
    const uint32_t ramp_ticks = 90U * PWM_MS;
    if (s.ticks <= ramp_ticks)
    {
      const float ramp = (float)s.ticks / (float)ramp_ticks;
      Servo_SetOpenloop((float)s.arg * 0.001f / s.vbus * ramp, 0.0f, 0.0f, s.fixed_elec);
    }
    if (s.ticks == 500U * PWM_MS)
      Servo_SetOpenloopRate((float)s.variant * 2.0f * FOC_TWO_PI);
  }
  else if (s.kind == DIR)
  {
    if (s.ticks == 4U * PWM_MS)
      Servo_SetVoltageCmd(0.0f, ((float)s.arg * 0.001f) / s.vbus);
    if (s.ticks % (5U * PWM_MS) == 0U)
      Push((uint16_t)Milli(motor_rad - s.origin), (uint16_t)Milli(motor_speed), raw);
  }
  else if (s.kind >= RES && s.kind <= LQ)
  {
    float v;
    if (s.kind == RES)
    {
      const float levels[] = {0.35f, 0.65f, 1.0f, 0.5f};
      v = levels[s.ticks / (200U * PWM_MS)] * (float)s.arg * 0.001f;
    }
    else
    {
      const uint32_t period = (s.ticks < (s.end / 2U)) ? 16U : 28U;
      v = s.ticks < 4U * PWM_MS ? 0.0f :
            ((s.ticks % period < period / 2U) ? 1.0f : -1.0f) * (float)s.arg * 0.001f;
    }
    s.vbus = Adc_GetVbusVolts();
    if (!InRange(s.vbus, CFG_VBUS_GAIN_MIN_V, 60.0f)) { End(FAULT); return 1U; }
    const float pu = Foc_Clamp(v / s.vbus, -CFG_V_LIMIT, CFG_V_LIMIT);
    s.d_pu = s.kind == LQ ? 0.0f : pu;
    s.q_pu = s.kind == LQ ? pu : 0.0f;
    if (s.kind == RES) { Servo_SetOpenloop(s.d_pu, 0.0f, 0.0f, s.fixed_elec); }
    else { Servo_SetVoltageCmd(s.d_pu, s.q_pu); }
  }
  if (s.kind >= VELOCITY && s.ticks % (5U * PWM_MS) == 0U)
  {
    ServoTelemetry_t tel;
    Servo_GetTelemetry(&tel);
    const uint16_t position = s.kind >= COG ? raw :
        (uint16_t)Milli((motor_rad - s.origin) * CFG_GEAR_RATIO_INV);
    Push(position, (uint16_t)Milli(motor_speed * CFG_GEAR_RATIO_INV), (uint16_t)Milli(tel.iq));
  }
  return s.state != RUN;
}

float Commission_LimitIq(float target, int8_t direction)
{
  if (s.state != RUN) { return target; }
  if (s.kind == CURRENT)
    return s.ticks < 10U * PWM_MS ? 0.0f : (float)(s.arg * direction) * 0.001f;
  return Foc_Clamp(target, -TRIAL_I_MAX, TRIAL_I_MAX);
}

uint8_t Commission_DAxisStep(void) { return s.state == RUN && s.kind == CURRENT; }

uint8_t Commission_Current(float elec, float ia, float ib, float iq_ref)
{
  if (s.state != RUN) { return 1U; }
  if (fabsf(ia) > 2.1f || fabsf(ib) > 2.1f || fabsf(ia + ib) > 2.1f)
  { End(CURRENT_LIMIT); return 0U; }
  if (!InRange(Adc_GetVbusVolts(), CFG_VBUS_GAIN_MIN_V, 60.0f))
  { End(SUPPLY); return 0U; }
  if ((s.kind == RES && s.ticks % 10U == 0U) ||
      (s.kind >= LD && s.kind <= CURRENT))
  {
    const Foc_Dq_t dq = Foc_Park(Foc_Clarke(ia, ib), elec);
    const float measured = (s.kind == LQ) ? dq.q : dq.d;
    Push((uint16_t)Milli(measured), (uint16_t)Milli(s.sampled_v),
         s.kind == CURRENT ? (uint16_t)Milli(iq_ref) : (uint16_t)s.ticks);
  }
  return s.state == RUN;
}

float Commission_Cogging(uint16_t raw)
{
  const uint32_t *w = s.state == RUN && s.kind == COG_CHECK ? s_stage : s_record.words;
  if (!(w[FLAGS] & HAS_COG) || (s.state == RUN && s.kind != COG_CHECK)) { return 0.0f; }
  const float idx = (float)raw * ((float)COG_POINTS / (float)ENCODER_CPR);
  const unsigned i = (unsigned)idx & (COG_POINTS - 1U);
  const float a = (float)CogAt(w, i);
  const float b = (float)CogAt(w, (i + 1U) & (COG_POINTS - 1U));
  /* Fade out above the low-speed region where this map was measured. */
  const float speed = fabsf(Foc_EncoderGetVelocity(Servo_GetEncoder()));
  return 0.001f * (a + (idx - (float)i) * (b - a)) * Foc_Clamp((12.0f - speed) / 7.0f, 0.0f, 1.0f);
}

static uint8_t Begin(const uint8_t *p, uint8_t calibrated)
{
  const uint8_t kind = p[0];
  const int8_t variant = (int8_t)p[1];
  const int16_t arg = (int16_t)Read16(p + 2);
  const uint16_t token = Read16(p + 4);
  const int amount = arg < 0 ? -(int)arg : (int)arg;
  if (Commission_Active()) { return BUSY; }
  if (DriveHeld() && token != s.token) { return BUSY; }
  if (Servo_GetFault()) { return FAULT; }
  if (!s_stage_ready || !Valid(s_stage) || !token || kind < ENC || kind > COG_CHECK) { return INVALID; }
  if (kind >= RES && !calibrated) { return NO_CALI; }
  if (kind == DIR && !(s_stage[FLAGS] & HAS_ENC)) { return NO_CALI; }
  if (kind >= CURRENT && Servo_GetInnerLoop() != CFG_INNER_LOOP_CURRENT) { return INVALID; }
  if ((kind == ENC && ((variant != 1 && variant != -1) || arg < 500 || arg > 9000)) ||
      (kind == DIR && (arg < 200 || arg > 3000)) ||
      (kind == RES && (arg < 500 || arg > 6000)) ||
      ((kind == LD || kind == LQ) && (arg < 150 || arg > 3000)) ||
      (kind == CURRENT && (amount < 100 || amount > 500)) ||
      (kind == VELOCITY && (amount < 100 || amount > 1200)) ||
      (kind == POSITION && (amount < 20 || amount > 400)) ||
      (kind >= COG && (amount < 70 || (float)amount * CFG_GEAR_RATIO > 4500.0f))) { return LIMIT; }
  (void)Servo_PauseForFlash();
  s.old_mode = Servo_GetCtrlMode();
  ReadControl(s.prior_control);
  if (kind == DIR) { ReadAlignment(&s.prior_alignment); }
  s.kind = kind;
  s.variant = variant;
  s.arg = arg;
  s.token = token;
  s_drive_held = 1U;
  s_drive_seen = Board_GetTickMs();
  s.was_calibrated = calibrated;
  s.lease = 400U * PWM_MS;
  s.preparation_ms = Board_GetTickMs();
  s.count = 0U;
  s.error = 0U;
  s.restored = 0U;
  if (kind == ENC) { s.end = PWM_MS * (1050U + 500U * CFG_POLE_PAIRS); }
  else if (kind == DIR) { s.end = 230U * PWM_MS; }
  else if (kind == RES) { s.end = 800U * PWM_MS; }
  else if (kind == LD || kind == LQ) { s.end = 60U * PWM_MS; }
  else if (kind == CURRENT) { s.end = 42U * PWM_MS; }
  else if (kind == POSITION) { s.end = 1800U * PWM_MS; }
  else if (kind == VELOCITY) { s.end = 900U * PWM_MS; }
  else
  {
    const float ms = 900.0f + 3.05f * FOC_TWO_PI / ((float)amount * 0.001f * CFG_GEAR_RATIO) * 1000.0f;
    s.end = (uint32_t)Foc_Clamp(ms, 2600.0f, 9800.0f) * PWM_MS;
  }
  s.state = PREPARE;
  return 0U;
}

static void Reply(uint32_t node, uint8_t op, uint8_t seq, const uint8_t *bytes, uint8_t error)
{
  uint8_t reply[8] = {op, seq};
  if (error != 0U) { reply[0] |= 0x80U; reply[2] = error; }
  else { memcpy(reply + 2, bytes, 6U); }
  (void)Can_Send(CFG_CAN_COMMISSION_BASE + node, reply);
}

uint8_t Commission_Handle(const uint8_t data[8], uint32_t node, uint8_t calibrated)
{
  const uint8_t *p = data + 2;
  uint8_t bytes[6] = {0};
  uint8_t error = 0U;
  uint8_t aligned = 0U;
  switch (data[0])
  {
    case INFO:
      if (p[0] == 0U)
      {
        bytes[0] = 1U;
        bytes[1] = BOARD_ID;
        bytes[2] = calibrated | (BOARD_HAS_ENCODER2 << 1) |
                   ((Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT) << 2);
        bytes[3] = CFG_POLE_PAIRS;
        bytes[4] = CFG_VERNIER_TEETH1;
        bytes[5] = CFG_VERNIER_TEETH2;
      }
      else if (p[0] == 1U)
      {
        bytes[0] = ENCODER_CPR == 65536U ? 16U : 14U;
        bytes[1] = (uint8_t)CFG_ENC2_DIR;
        Put16(bytes + 2, (uint16_t)(CFG_GEAR_RATIO * 1000.0f));
        Put16(bytes + 4, (uint16_t)(CFG_VERNIER_RESIDUE_TOL * 1000.0f));
      }
      else if (p[0] == 2U)
      {
        Put16(bytes, (uint16_t)Foc_Clamp(1000.0f * Adc_GetVbusVolts(), 0.0f, 65535.0f));
        Put16(bytes + 2, (uint16_t)Milli(CFG_MOTOR_KT_NM_A));
        bytes[4] = (uint8_t)s_record.words[FLAGS];
        bytes[5] = (Servo_GetWarning() & SERVO_WARN_ABS_UNKNOWN) != 0U;
      }
      else if (p[0] == 3U) { Put32(bytes, Bits(Servo_GetZeroMotor())); }
      else { error = INVALID; }
      break;
    case START:
      error = Begin(p, calibrated);
      break;
    case STATUS:
    {
      const uint32_t lock = Board_IrqLock();
      if (Read16(p) != s.token || s.token == 0U) { error = INVALID; }
      else
      {
        s.lease = 400U * PWM_MS;
        s_drive_seen = Board_GetTickMs();
        bytes[0] = s.state;
        bytes[1] = s.kind;
        Put16(bytes + 2, s.count);
        bytes[4] = s.error;
        bytes[5] = s.restored;
      }
      Board_IrqUnlock(lock);
      break;
    }
    case SAMPLE:
    {
      const uint16_t i = Read16(p);
      if (Read16(p + 2) != s.token || s.state != DONE || !s.restored || i >= s.count) { error = DATA; }
      else
      {
        s_drive_seen = Board_GetTickMs();
        Put16(bytes, s_samples[i].a);
        Put16(bytes + 2, s_samples[i].b);
        Put16(bytes + 4, s_samples[i].c);
      }
      break;
    }
    case STOP:
      Commission_Abort();
      break;
    case READ:
      if (Commission_Active()) { error = BUSY; }
      else if (p[0] >= WORDS || !s_stage_ready) { error = INVALID; }
      else { Put32(bytes, s_stage[p[0]]); }
      break;
    case WRITE:
      if (Commission_Active()) { error = BUSY; }
      else if (p[0] >= WORDS || !s_stage_ready) { error = INVALID; }
      else { s_stage[p[0]] = Read32(p + 2); }
      break;
    case PROFILE:
      if (Commission_BlocksDrive()) { error = BUSY; }
      else if (p[0] == 0U) { StageFromLive(calibrated); }
      else if (p[0] != 1U || !s_stage_ready || !Valid(s_stage) ||
               Read32(p + 2) != Crc(s_stage, sizeof(s_stage))) { error = INVALID; }
      else if (Servo_GetFault() || !Servo_EncoderStable()) { error = FAULT; }
      else
      {
        CaliNvData_t nv = {0};
        const uint8_t legacy_valid = CaliNv_Load(&nv);
        const uint32_t removed = s_record.words[FLAGS] & ~s_stage[FLAGS];
        const ServoMode_t previous = Servo_PauseForFlash();
        const uint8_t angle_changed = (s_stage[FLAGS] & HAS_ENC) &&
            ((s_record.words[FLAGS] & (HAS_ENC | HAS_AUX)) != (s_stage[FLAGS] & (HAS_ENC | HAS_AUX)) ||
             s_stage[ENCODER] != s_record.words[ENCODER] || s_stage[OFFSET] != s_record.words[OFFSET]);
        if (!Save(s_stage)) { error = STORAGE; }
        else
        {
          /* Invalidating a tune restores the same fallback as a cold boot.
           * Staging and failed flash writes do not change the live gains. */
          if (removed & HAS_CURRENT)
          {
            const float wc = FOC_TWO_PI * CFG_CUR_BW_HZ;
            Servo_SetCurrentGains(CFG_MOTOR_L_H * wc, CFG_MOTOR_R_OHM * wc);
          }
          if (removed & HAS_PID) { Servo_ResetFeedbackGains(); }
          if (angle_changed || (removed & HAS_PID))
          {
            const uint8_t have_alignment = Commission_MergeAlignment(&nv, legacy_valid);
            if (angle_changed)
            {
              if (!have_alignment || !Cali_ApplySaved(&nv)) { error = FAULT; }
              else { aligned = 1U; }
            }
            else if (have_alignment) { Cali_ApplyUserGains(&nv); }
          }
          if (error == 0U) { Commission_ApplyControl(); }
        }
        if (!aligned) { Servo_ResumeAfterFlash(previous); }
      }
      break;
    default: error = INVALID; break;
  }
  Reply(node, data[0], data[1], bytes, error);
  return aligned;
}
