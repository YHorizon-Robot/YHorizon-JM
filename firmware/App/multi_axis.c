#include "multi_axis.h"
#include "board.h"
#include "can.h"
#include "config.h"
#include "servo.h"
#include <math.h>
#include <string.h>

enum { IDLE, STAGING, ARMED, ACTIVE };
enum { CAPS = 0x40, TARGET, ARM, SECOND, LIMITS };
enum { START = 1, BEAT, STOP };
enum { INVALID = 1, BUSY, NOT_READY, LIMIT };
#define TICKS_PER_BEAT (CFG_CTRL_HZ / 200U)
_Static_assert(CFG_CTRL_HZ == 4000U, "sync feedback slots use 250 us outer ticks");

static struct {
  volatile uint8_t state, abort, edge, started;
  volatile uint8_t feedback_pending;
  uint8_t node, slot, old_mode, two;
  uint16_t token, duration, cycle, age;
  uint32_t phase, ticks, send_at, staged_ms;
  float origin, end[2], inv_ticks, inv_seconds;
  uint8_t feedback[8];
} s;

static uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void Put16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void Put32(uint8_t *p, uint32_t n) { Put16(p, (uint16_t)n); Put16(p+2, (uint16_t)(n >> 16)); }
static int32_t Millirad(float x) { return (int32_t)roundf(x * 1000.0f); }

uint8_t MultiAxis_Busy(void) { return s.state != IDLE; }

void MultiAxis_Stop(void)
{
  const uint32_t lock = Board_IrqLock();
  const uint8_t restore = s.state >= ARMED;
  const uint8_t old = s.old_mode;
  s.state = IDLE;
  s.edge = s.abort = s.started = s.feedback_pending = 0U;
  if (restore) { Servo_SetCtrlMode(old); } /* clears targets and closes bridge */
  Board_IrqUnlock(lock);
}

static uint8_t Fits(float distance, float seconds)
{
  return fabsf(distance) <= 30.0f && 1.875f * fabsf(distance) <= 30.0f * seconds &&
         5.774f * fabsf(distance) <= CFG_POS_ACC_DEFAULT * seconds * seconds;
}

void MultiAxis_Command(const uint8_t d[8], uint8_t node, uint8_t calibrated)
{
  uint8_t r[8] = {d[0], d[1], 0, 1};
  const uint16_t token = U16(d+2);
  ServoTelemetry_t t;
  Servo_GetTelemetry(&t);
  const int32_t here = Millirad(t.p_act);
  Put32(r+4, (uint32_t)here);
  const uint32_t lock = Board_IrqLock();
  if (!calibrated || Servo_GetFault() || Servo_GetMode() != SERVO_RUN ||
      Servo_GetInnerLoop() != CFG_INNER_LOOP_CURRENT || !Servo_EncoderStable() ||
      (Servo_GetWarning() & SERVO_WARN_ABS_UNKNOWN)) { r[2] = NOT_READY; }
  else if (d[0] == CAPS) { if (s.state != IDLE) { r[2] = BUSY; } }
  else if (d[0] == LIMITS)
  { Put16(r+4, 30000U); Put16(r+6, (uint16_t)fminf(CFG_POS_ACC_DEFAULT * 1000.0f, 65535.0f)); }
  else if (token == 0U) { r[2] = INVALID; }
  else if (d[0] == TARGET || d[0] == SECOND)
  {
    const float goal = (float)(int32_t)((uint32_t)U16(d+4) | ((uint32_t)U16(d+6) << 16)) * 0.001f;
    if (d[0] == TARGET && s.state == IDLE)
    {
      Servo_ResumeAfterFlash(Servo_PauseForFlash());
      s.token = token; s.node = node; s.two = 0U; s.end[0] = goal;
      s.state = STAGING; s.staged_ms = Board_GetTickMs();
    }
    else if (s.state != STAGING || token != s.token) { r[2] = BUSY; }
    else if (d[0] == SECOND) { s.end[1] = goal; s.two = 1U; }
    else if (s.end[0] != goal) { r[2] = INVALID; }
  }
  else if (d[0] == ARM)
  {
    const uint16_t duration = U16(d+4);
    const float seconds = (float)duration * 0.005f;
    float kp, ki, kd, vkp, vki;
    Servo_GetPositionGains(&kp, &ki, &kd);
    Servo_GetVelocityGains(&vkp, &vki);
    if (token != s.token || (s.state != STAGING && s.state != ARMED)) { r[2] = BUSY; }
    else if (s.state == ARMED) {
      if (duration != s.duration || d[6] != s.slot || d[7] != s.two) { r[2] = INVALID; }
      Put32(r+4, (uint32_t)Millirad(s.origin));
    }
    else if (duration < 40U || duration > 12000U || d[6] >= 16U || d[7] != s.two ||
             !Fits(s.end[0]-t.p_act, seconds) ||
             (s.two && (!Fits(s.end[1]-s.end[0], seconds) || fabsf(s.end[1]-t.p_act)>30.0f))) { r[2] = LIMIT; }
    else if (kp <= 0.0f || (vkp <= 0.0f && vki <= 0.0f) || fabsf(t.v_act) > 0.25f) { r[2] = NOT_READY; }
    else
    {
      s.old_mode = Servo_GetCtrlMode();
      Servo_SetCtrlMode(SERVO_CTRL_POSITION);
      s.origin = (float)here * 0.001f; s.slot = d[6]; s.duration = duration;
      s.inv_ticks = 1.0f / (float)(duration*TICKS_PER_BEAT); s.inv_seconds = 1.0f/seconds;
      s.phase = s.ticks = s.age = s.started = s.edge = s.abort = s.feedback_pending = 0U;
      s.staged_ms = Board_GetTickMs(); s.state = ARMED;
    }
  }
  else { r[2] = INVALID; }
  Board_IrqUnlock(lock);
  (void)Can_Send(CFG_CAN_SYNC_REPLY + node, r);
}

void MultiAxis_OnSyncIrq(const uint8_t d[8])
{
  const uint32_t lock = Board_IrqLock();
  if (s.state != IDLE && U16(d+2) == s.token && d[1] == 0U && U16(d+6) == 0U)
  {
    const uint16_t cycle = U16(d+4);
    if (d[0] == STOP) { s.abort = 1U; }
    else if (d[0] == START && !cycle && s.state == ARMED && !s.started)
    { s.started = 1U; s.cycle = 0U; s.edge = 1U; }
    else if (d[0] == BEAT && s.state == ACTIVE && cycle > s.cycle && cycle - s.cycle <= 5)
    { s.cycle = cycle; s.edge = 1U; }
  }
  Board_IrqUnlock(lock);
}

uint8_t MultiAxis_Control(float p, float v, float *ref, float *ff)
{
  /* Called from the outer-loop IRQ, above the CAN IRQ in priority. */
  if (s.abort || Servo_GetMode() != SERVO_RUN || Servo_GetFault() ||
      (s.state != ACTIVE && Board_GetTickMs() - s.staged_ms >= 2000U))
  { MultiAxis_Stop(); return 0U; }
  if (!s.started) { return 0U; }
  if (s.state == ARMED && (fabsf(p - s.origin) > 0.015f || fabsf(v) > 0.25f))
  { MultiAxis_Stop(); return 0U; }
  s.state = ACTIVE;
  s.ticks++;
  unsigned increment = 1U;
  if (s.edge)
  {
    s.edge = 0U; s.age = 0U;
    /* Correct slow clock drift without jumping/reversing the reference. */
    const uint32_t target = (uint32_t)s.cycle * TICKS_PER_BEAT;
    increment = s.phase < target ? 2U : (s.phase > target ? 0U : 1U);
    s.send_at = s.ticks + s.slot;
    const int32_t displacement = Millirad(p - s.origin);
    const int32_t speed = Millirad(v);
    if (displacement < -32768 || displacement > 32767 || speed < -32768 || speed > 32767)
    { MultiAxis_Stop(); return 0U; }
    Put16(s.feedback, s.token); Put16(s.feedback+2,s.cycle);
    Put16(s.feedback+4,(uint16_t)displacement); Put16(s.feedback+6,(uint16_t)speed);
    s.feedback_pending = 1U;
  }
  if (++s.age >= 120U) { MultiAxis_Stop(); return 0U; }
  const uint32_t span = (uint32_t)s.duration * TICKS_PER_BEAT;
  const uint8_t seg = s.two && s.phase >= span;
  const uint32_t within = s.phase - (seg ? span : 0U);
  const float u = within >= span ? 1.0f : (float)within * s.inv_ticks;
  const float from = seg ? s.end[0] : s.origin;
  const float delta = s.end[seg] - from;
  *ref = from + delta * (u*u*u*(10.0f+u*(-15.0f+6.0f*u)));
  *ff = delta * s.inv_seconds * (30.0f*u*u*(1.0f-u)*(1.0f-u));
  if (fabsf(*ref - p) > 0.5f) { MultiAxis_Stop(); return 0U; }
  if (s.phase < span * (1U + s.two)) { s.phase += increment; }
  return 1U;
}

void MultiAxis_Service(void)
{
  if (!s.feedback_pending) { return; }
  const uint32_t lock = Board_IrqLock();
  if (s.feedback_pending && s.state == ACTIVE && s.ticks >= s.send_at)
  { if (Can_Send(CFG_CAN_SYNC_FB + s.node, s.feedback)) { s.feedback_pending = 0U; } }
  Board_IrqUnlock(lock);
}
