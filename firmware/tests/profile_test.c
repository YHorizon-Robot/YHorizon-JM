/* Test complete Comm entry points against fixed wire examples and simulated hardware. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "profile.h"
#include "adc.h"
#include "board.h"
#include "comm.h"
#include "commission.h"
#include "config.h"
#include "debug.h"
#include "node.h"
#include "servo.h"

#define FRAME(id_, ext_, len_, ...) { .id=id_, .extended=ext_, .dlc=len_, .data={__VA_ARGS__} }
#if CFG_RS05 || CFG_CYBERGEAR
static const CanFrame_t arm = FRAME(0x0300FD02,1,8,0);
static const CanFrame_t move = FRAME(0x01FFFF02,1,8,0,0,255,255,255,255,0,0);
static const CanFrame_t stop = FRAME(0x0400FD02,1,8,0);
#elif CFG_AK60_6
static const CanFrame_t arm = FRAME(2,0,8,255,255,255,255,255,255,255,0xFC);
static const CanFrame_t move = FRAME(2,0,8,0,0,255,255,255,0,15,255);
static const CanFrame_t stop = FRAME(2,0,8,255,255,255,255,255,255,255,0xFD);
#elif CFG_RMD_X || CFG_LK_MF
#if CFG_RMD_X
static const CanFrame_t arm = FRAME(0x142,0,8,0xA1);
#else
static const CanFrame_t arm = FRAME(0x142,0,8,0x88);
#endif
static const CanFrame_t move = FRAME(0x142,0,8,0xA4,0,10,0,0x18,0xFC,0xFF,0xFF);
static const CanFrame_t stop = FRAME(0x142,0,8,0x80);
#elif CFG_HIGHTORQUE
static const CanFrame_t arm = FRAME(0x8002,1,4,5,0x13,0,0);
static const CanFrame_t move = FRAME(0x8002,1,4,5,0x13,0x9C,0xFF);
static const CanFrame_t stop = FRAME(0x8002,1,3,1,0,0);
#else
/* Node 2: other slots already contain non-neutral data. */
static const CanFrame_t arm = FRAME(0x200,0,8,0x20,0,0,0,0x20,0,0x20,0);
static const CanFrame_t move = FRAME(0x200,0,8,0x20,0,0xFC,0x18,0x20,0,0x20,0);
static const CanFrame_t stop = FRAME(0x200,0,8,0x20,0,0,0);
#endif

static uint32_t tick;
static uint8_t node = 2, ctrl_mode, lost, stable = 1, busy, warning, adc_ready = 1;
static uint16_t fault;
static unsigned count, consumed, moves, gates, replies, lock_depth;
static float vbus = 24, position, velocity, applied_p, applied_v, demand, kp, kd;
static CanFrame_t queue[16], reply;
uint8_t Node_GetId(void) { return node; }
uint32_t Board_GetTickMs(void) { return tick; }
void Board_GetUid64(uint8_t uid[8]) { memset(uid, 0x35, 8); }
uint32_t Board_IrqLock(void) { return lock_depth++; }
void Board_IrqUnlock(uint32_t prev) { assert(lock_depth == prev + 1); lock_depth = prev; }
void Dbg_Printf(const char *fmt, ...) { (void)fmt; }
uint8_t Commission_BlocksDrive(void) { return busy; }
uint8_t Adc_CurrentSenseReady(void) { return adc_ready; }
float Adc_GetVbusVolts(void) { return vbus; }
uint8_t Servo_EncoderStable(void) { return stable; }
uint16_t Servo_GetFault(void) { return fault; }
uint8_t Servo_GetWarning(void) { return warning; }
ServoMode_t Servo_GetMode(void) { return fault ? SERVO_FAULT : SERVO_RUN; }
uint8_t Servo_GetInnerLoop(void) { return CFG_INNER_LOOP_CURRENT; }
ServoMode_t Servo_PauseForFlash(void) { ++gates; return Servo_GetMode(); }
void Servo_ResumeAfterFlash(ServoMode_t previous) { (void)previous; }
uint8_t Servo_ClearFault(void) { fault = 0; return 1; }
static float velocity_gain = 1;
void Servo_GetVelocityGains(float *p, float *i) { *p = velocity_gain; *i = 0; }
void Servo_GetPositionGains(float *p, float *i, float *d) { *p = 1; *i = 0; if (d) { *d = 0; } }
uint8_t Servo_GetCtrlMode(void) { return ctrl_mode; }
void Servo_SetCtrlMode(uint8_t m) { assert(lock_depth); ctrl_mode = m; }
void Servo_SetMotion(float p, float v, float ff)
{ assert(lock_depth && !lost); ++moves; applied_p = p; applied_v = v; demand = ff; }
void Servo_SetVelocityCmd(float v) { assert(lock_depth); ++moves; applied_v = v; }
void Servo_SetPositionCmd(float p, float v) { assert(lock_depth); ++moves; applied_p = p; applied_v = v; }
void Servo_SetGains(float a, float b) { assert(lock_depth); kp = a; kd = b; }
void Servo_GetTelemetry(ServoTelemetry_t *t)
{
  *t = (ServoTelemetry_t){.p_act=position, .v_act=velocity, .iq=1, .torque_nm=0.23f, .closed_loop_dir=-1};
}
Foc_Encoder_t *Servo_GetEncoder(void) { return NULL; }
float Foc_EncoderGetMechWrapped(const Foc_Encoder_t *enc) { (void)enc; return RAD_PER_TURN/4; }
uint8_t Servo_GetTemperature(float *temp) { *temp = 35; return 1; }
void Can_StopForFlash(void) { Comm_OnCanLoss(); }
void Can_Restart(void) { consumed = count = lost = 0; }
uint8_t Can_RxLost(void) { return lost; }
uint8_t Can_PopRx(CanFrame_t *out)
{
  if (consumed == count) { return 0; }
  *out = queue[consumed++ % 16]; return 1;
}
uint8_t Can_SendFrame(const CanFrame_t *f) { reply = *f; ++replies; return 1; }
static void Push(CanFrame_t f)
{
  assert(count - consumed < 16); f.received_ms = tick; queue[count++ % 16] = f;
}
static void Send(CanFrame_t f) { Push(f); Comm_Process(); }
static void ArmAndMove(void) { Push(arm); Send(move); }
static void Near(float a, float b) { assert(fabsf(a-b) < 0.0001f); }

static void TestSession(void)
{
  Push(arm); Push(move); Comm_Init(1); Comm_Process();
  assert(moves == 0);
  Send(move); assert(moves == 0);
  stable = 0; ArmAndMove(); stable = 1;
  busy = 1; ArmAndMove(); busy = 0;
  warning = SERVO_WARN_ABS_UNKNOWN; ArmAndMove(); warning = 0;
  vbus = 0; ArmAndMove(); vbus = 24;
  adc_ready = 0; ArmAndMove(); adc_ready = 1;
  Comm_Init(0); ArmAndMove(); assert(!Comm_IsCalibrated() && !moves);
  Comm_Init(1); node = 0; Comm_Init(1); ArmAndMove(); assert(!moves);
  node = 2; Comm_Init(1); tick = UINT32_MAX-40U;
  CanFrame_t bad = arm;
  bad.extended ^= 1U; Send(bad);
  bad = arm; bad.dlc = 1; Send(bad);
  bad = arm; bad.id ^= 1U; Send(bad);
  Send(move); assert(!moves);
  ArmAndMove(); assert(moves == 1);
#if CFG_RMD_X || CFG_LK_MF
  assert(ctrl_mode == SERVO_CTRL_POSITION);
  Near(applied_p, -10*RAD_PER_DEG); Near(applied_v, 10*RAD_PER_DEG);
#elif CFG_RS05
  Near(applied_p, -12.57f); Near(applied_v, 33); Near(demand, Cfg_NmToPu(14));
#elif CFG_CYBERGEAR
  Near(applied_p, -2*RAD_PER_TURN); Near(applied_v, 30); Near(demand, Cfg_NmToPu(12));
#elif CFG_AK60_6
  Near(applied_p, -12.5f); Near(applied_v, 45); Near(demand, Cfg_NmToPu(15));
#elif CFG_HIGHTORQUE
  Near(demand, Cfg_NmToPu(-1));
#elif CFG_C620
  Near(demand, -1000.0f * 20/16384 * CFG_V_LIMIT/CFG_I_LIMIT_A);
#else
  Near(demand, -CFG_V_LIMIT/CFG_I_LIMIT_A);
#endif
  Push(move); Send(stop); assert(moves == 1 && gates);
  Comm_OnCanLoss(); Send(move); assert(moves == 1);
  ArmAndMove(); tick += 99; bad = move; bad.extended ^= 1U; Send(bad);
  unsigned before = gates;
  ++tick; Send(move); assert(moves == 2 && gates == before+1);
  Push(arm); tick += 100; Send(move); assert(moves == 2);
  ArmAndMove(); fault = SERVO_FAULT_ENCODER; Send(move); fault = 0;
  Send(move); assert(moves == 3);
  ArmAndMove(); lost = 1; Send(move); Can_StopForFlash(); Can_Restart();
  Send(move); assert(moves == 4);
}

static void TestWire(void)
{
  Comm_Init(1);
#if CFG_RS05 || CFG_CYBERGEAR
  CanFrame_t ping = FRAME(0x0000FD02,1,8,0);
  Send(ping); assert(reply.id == 0x2FE && reply.extended && reply.dlc == 8 && reply.data[7] == 0x35);
  CanFrame_t query = FRAME(0x1100FD02,1,8,0x19,0x70);
  CanFrame_t zero = FRAME(0x0600FD02,1,8,1);
  position = 1; Send(zero); Send(query);
  assert(reply.id == 0x110002FD && BytesZero(reply.data+4, 4));
  ArmAndMove(); Near(applied_p, 1.0f +
#if CFG_RS05
    -12.57f
#else
    -2*RAD_PER_TURN
#endif
  );
  Send(stop); Comm_Init(1); Send(query); assert(reply.data[6] == 0x80 && reply.data[7] == 0x3F);
  unsigned sent = replies;
  query.id ^= 0x100; Send(query);
  query.id = 0x1200FD02; Send(query); assert(replies == sent);
  ArmAndMove(); ++tick; Send(query); query.id = 0x1100FD02;
  tick += 98; Send(query); ++tick; unsigned before = moves; Send(move); assert(moves == before);
#elif CFG_RMD_X || CFG_LK_MF
  position = -10*RAD_PER_DEG; velocity = -20*RAD_PER_DEG;
  CanFrame_t q = FRAME(0x142,0,8,0x92);
  Send(q);
#if CFG_RMD_X
  assert(reply.id == 0x242 && BytesZero(reply.data+1, 3) && ReadLE16(reply.data+4)==0xFC18);
#else
  assert(reply.id == 0x142 && ReadLE16(reply.data+1)==0xFC18 && reply.data[7]==0xFF);
#endif
  velocity_gain = 0; unsigned initial = moves;
  ArmAndMove(); assert(moves == initial);
  velocity_gain = 1;
  q.data[0]=0x9C; Send(q);
  assert(reply.dlc==8 && !reply.extended && reply.data[1]==35 && (int16_t)ReadLE16(reply.data+4)==-20);
#if CFG_RMD_X
  assert((int16_t)ReadLE16(reply.data+2)==-100 && (int16_t)ReadLE16(reply.data+6)==-10);
#else
  assert((int16_t)ReadLE16(reply.data+2)==-62 && ReadLE16(reply.data+6)==15928);
#endif
  CanFrame_t v = FRAME(0x142,0,8,0xA2,0,0,0,0x10,0x27);
  Send(arm); Send(v); Near(applied_v, 100*RAD_PER_DEG); assert(ctrl_mode == SERVO_CTRL_VELOCITY);
  v.data[0]=0xA1; v.data[4]=100; v.data[5]=0; Send(v);
#if CFG_RMD_X
  Near(demand, CFG_V_LIMIT/CFG_I_LIMIT_A);
#else
  Near(demand, 100*16.5f/2048 * CFG_V_LIMIT/CFG_I_LIMIT_A);
#endif
#elif CFG_AK60_6
  Send(arm); assert(!reply.extended && reply.id==2 && reply.dlc==8 && reply.data[0]==2 &&
                     reply.data[6]==75 && reply.data[7]==0);
  unsigned sent = replies;
  CanFrame_t bad = arm; bad.data[7] = 0xFB; Send(bad); assert(replies == sent);
#elif CFG_HIGHTORQUE
  CanFrame_t q = FRAME(0x8002,1,2,0x17,1);
  position=RAD_PER_TURN/2; velocity=-RAD_PER_TURN/4;
  Send(q); assert(reply.extended && reply.id == 0x200 && reply.dlc==8 &&
                  reply.data[0]==0x27 && ReadLE16(reply.data+2)==5000 &&
                  (int16_t)ReadLE16(reply.data+4)==-1000 && (int16_t)ReadLE16(reply.data+6)==-184);
  unsigned sent = replies; q.dlc=8; Send(q); assert(replies == sent+1);
  q.data[2]=42; Send(q); q.data[2]=0; q.id=2; Send(q); assert(replies==sent+1);
#else
  node=6; Comm_Init(1);
  unsigned before=moves; ArmAndMove(); assert(moves==before);
  CanFrame_t a=arm, m=move; a.id=m.id=0x1FF;
  Push(a); Send(m); assert(moves==before+1);
  unsigned sent=replies; velocity=RAD_PER_TURN; ++tick; Comm_Process(); Comm_Process();
  assert(replies==sent+1 && !reply.extended && reply.id==0x206 && reply.dlc==8 &&
         ReadBE16(reply.data)==2048 && ReadBE16(reply.data+2)==480 && reply.data[7]==0);
#if CFG_C620
  assert((int16_t)ReadBE16(reply.data+4)==-819 && reply.data[6]==35);
#else
  assert((int16_t)ReadBE16(reply.data+4)==-1000 && reply.data[6]==0);
#endif
#endif
}
int main(void) { TestSession(); TestWire(); return 0; }
