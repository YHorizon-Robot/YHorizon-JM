/* Exercise the firmware entry points without opening a CAN device. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <string.h>

#include "adc.h"
#include "board.h"
#include "can.h"
#include "comm.h"
#include "commission.h"
#include "config.h"
#include "debug.h"
#include "dm4310.h"
#include "node.h"
#include "servo.h"

static CanFrame_t queue[32];
static unsigned head, tail, sent, moves, stops, irq_depth;
static uint32_t tick;
static uint8_t node = 1, lost, stable = 1, adc_ready = 1, busy, warning;
static uint16_t fault;
static float vbus = 24.0f;
static ServoMode_t mode = SERVO_RUN;
static uint8_t response[8];
static DmMotion applied;

uint32_t Board_GetTickMs(void) { return tick; }
uint32_t Board_IrqLock(void) { return irq_depth++; }
void Board_IrqUnlock(uint32_t saved) { assert(irq_depth == saved + 1U); irq_depth = saved; }
uint8_t Node_GetId(void) { return node; }
void Dbg_Printf(const char *fmt, ...) { (void)fmt; }
uint8_t Commission_BlocksDrive(void) { return busy; }
uint8_t Adc_CurrentSenseReady(void) { return adc_ready; }
float Adc_GetVbusVolts(void) { return vbus; }
uint8_t Servo_EncoderStable(void) { return stable; }
uint16_t Servo_GetFault(void) { return fault; }
uint8_t Servo_GetWarning(void) { return warning; }
ServoMode_t Servo_GetMode(void) { return mode; }
uint8_t Servo_GetInnerLoop(void) { return CFG_INNER_LOOP_CURRENT; }
ServoMode_t Servo_PauseForFlash(void) { ++stops; return mode; }
void Servo_ResumeAfterFlash(ServoMode_t previous) { if (!fault) { mode = previous; } }
uint8_t Servo_ClearFault(void)
{
  if (!adc_ready) { return 0U; }
  fault = 0U; mode = SERVO_IDLE;
  return 1U;
}
void Servo_SetGains(float kp, float kd)
{
  assert(irq_depth); applied.kp = kp; applied.kd = kd;
}
void Servo_SetMotion(float p, float v, float ff)
{
  assert(irq_depth); ++moves;
  applied.position = p; applied.velocity = v; applied.torque = ff;
}
void Servo_GetTelemetry(ServoTelemetry_t *out)
{
  memset(out, 0, sizeof(*out));
  out->torque_nm = 0.23f; out->closed_loop_dir = -1;
}
uint8_t Servo_GetTemperature(float *out) { *out = 35.0f; return 1; }
void Can_StopForFlash(void) { Comm_OnCanLoss(); }
void Can_Restart(void) { head = tail = 0U; lost = 0U; }
uint8_t Can_RxLost(void) { return lost; }
uint8_t Can_PopRx(CanFrame_t *out)
{
  if (tail == head) { return 0; }
  *out = queue[tail++ % 32];
  return 1;
}
uint8_t Can_Send(uint32_t id, const uint8_t d[8])
{
  assert(id == Dm_FeedbackId(node)); ++sent; memcpy(response, d, 8); return 1;
}
static void Push(uint32_t id, const uint8_t d[8])
{
  CanFrame_t *f = &queue[head++ % 32];
  assert(head - tail <= 32); f->id = id; f->received_ms = tick; memcpy(f->data, d, 8);
}
static void SystemCommand(uint8_t code)
{
  uint8_t d[8]; memset(d, 0xFF, 8); d[7] = code; Push(node, d);
}
/* Golden bytes: P=min, V=max, Kp=4095/4095, Kd=0, torque=max. */
static const uint8_t motion[8] = {0, 0, 0xFF, 0xFF, 0xFF, 0, 0x0F, 0xFF};
static void ArmAndMove(void)
{
  SystemCommand(DM_ENABLE); Push(node, motion); Comm_Process();
}

static void TestMotionAndBarriers(void)
{
  SystemCommand(DM_ENABLE); Push(node, motion);
  Comm_Init(1); Comm_Process(); assert(moves == 0 && sent == 0);
  Push(node, motion); Comm_Process(); assert(moves == 0 && response[0] == 1);
  ArmAndMove();
  assert(moves == 1 && response[0] == 0x11 && response[6] == 35 && response[7] == 0);
  assert(fabsf(applied.position + 12.5f) < 1e-5f && fabsf(applied.velocity - 30) < 1e-5f);
  assert(fabsf(applied.kp - Cfg_NmToPu(500)) < 1e-5f && applied.kd == 0);
  assert(fabsf(applied.torque - Cfg_NmToPu(10)) < 1e-6f);
  assert((((response[4] & 15) << 8) | response[5]) == 1670); /* -1.84 Nm */
  Push(node, motion); SystemCommand(DM_DISABLE); Push(node, motion); Comm_Process();
  assert(moves == 1 && stops == 1 && response[0] == 1);
  ArmAndMove(); SystemCommand(DM_ZERO); Push(node, motion); Comm_Process();
  assert(moves == 2 && stops == 2);
  SystemCommand(DM_ENABLE); tick += DM4310_TIMEOUT_MS; Push(node, motion); Comm_Process();
  assert(moves == 2); /* stale enable */
  ArmAndMove(); lost = 1; Push(node, motion); Comm_Process();
  Can_StopForFlash(); Can_Restart(); Push(node, motion); Comm_Process();
  assert(moves == 3 && response[0] == 0xD1);
}

static void TestWatchdogAndRegisters(void)
{
  const unsigned initial = moves;
  const uint8_t query[8] = {1, 0, 0x33, 9, 0, 0, 0, 0};
  const uint8_t expected[8] = {1, 0, 0x33, 9, 0xD0, 7, 0, 0};
  tick = UINT32_MAX - 40U;
  Comm_Init(1); ArmAndMove(); tick += DM4310_TIMEOUT_MS - 1;
  Push(DM4310_REGISTER_ID, query); SystemCommand(DM_ENABLE); Comm_Process();
  assert(response[0] == 0x11);
  Push(DM4310_REGISTER_ID, query); Comm_Process(); assert(!memcmp(response, expected, 8));
  ++tick; Push(node, motion); Comm_Process();
  assert(response[0] == 0xD1 && moves == initial + 1);
  unsigned replies = sent;
  uint8_t bad[8]; memcpy(bad, query, 8); bad[0]++; Push(DM4310_REGISTER_ID, bad);
  memcpy(bad, query, 8); bad[2] = 0x55; Push(DM4310_REGISTER_ID, bad);
  SystemCommand(DM_RESERVED); Push(node + 1U, motion); Comm_Process(); assert(sent == replies);
  uint8_t reply[8];
  memcpy(bad, query, 8); bad[3] = 21;
  assert(Dm_ReadRegister(node, bad, reply) && reply[6] == 0x48 && reply[7] == 0x41);
}

static void TestInterlocks(void)
{
  unsigned initial = moves;
  Comm_Init(0); ArmAndMove(); SystemCommand(DM_CLEAR); Comm_Process();
  assert(!Comm_IsCalibrated() && moves == initial);
  node = 16; Comm_Init(1); unsigned replies = sent; ArmAndMove();
  assert(sent == replies && moves == initial);
  node = 1; Comm_Init(1);
  stable = 0; ArmAndMove(); stable = 1;
  warning = SERVO_WARN_ABS_UNKNOWN; ArmAndMove(); warning = 0;
  busy = 1; ArmAndMove(); busy = 0;
  vbus = 0; ArmAndMove(); vbus = 24;
  assert(moves == initial);
  ArmAndMove(); fault = SERVO_FAULT_OVERCURRENT; mode = SERVO_FAULT;
  Push(node, motion); Comm_Process(); assert(response[0] == 0xA1);
  adc_ready = 0; SystemCommand(DM_CLEAR); Comm_Process(); assert(fault);
  adc_ready = 1; SystemCommand(DM_CLEAR); Push(node, motion); Comm_Process();
  assert(!fault && moves == initial + 1);
  ArmAndMove(); assert(moves == initial + 2);
}

int main(void)
{
  TestMotionAndBarriers();
  TestWatchdogAndRegisters();
  TestInterlocks();
  return 0;
}
