/* Exercise the real calibration loader, commissioning commit and current loop
 * against RAM-backed flash and a stationary encoder, without driving hardware. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <string.h>
#include "adc.h"
#include "board.h"
#include "cali.h"
#include "can.h"
#include "commission.h"
#include "comm.h"
#include "config.h"
#include "debug.h"
#include "encoder.h"
#include "foc_math.h"
#include "nv_flash.h"
#include "pwm.h"
#include "servo.h"

enum { PAGE = 2048, WORDS = 144, TABLE = 16 };
static uint8_t flash[3 * PAGE], reply[8], bridge, fail_write;
static uint32_t tick, writes;

#if !CFG_CAN_COMPAT
static CanFrame_t rx[3];
static unsigned rx_count, rx_tail;
static uint32_t reply_id;
uint8_t Node_GetId(void) { return 1U; }
uint8_t Node_ApplyAndReset(uint8_t id) { (void)id; return 0U; }
uint8_t Can_PopRx(CanFrame_t *frame)
{
  if (rx_tail == rx_count) { rx_tail = rx_count = 0; return 0; }
  *frame = rx[rx_tail++];
  return 1;
}
static void Queue(CanFrame_t frame)
{
  assert(rx_count < sizeof(rx) / sizeof(rx[0]));
  rx[rx_count++] = frame;
}
#endif

uint32_t Board_GetTickMs(void) { return tick; }
void Board_DelayMs(uint32_t ms) { tick += ms; }
uint32_t Board_IrqLock(void) { return 0; }
void Board_IrqUnlock(uint32_t lock) { (void)lock; }
void Dbg_Printf(const char *fmt, ...) { (void)fmt; }
uint32_t NvFlash_PageAddr(void) { return BOARD_NV_PAGE_ADDR; }
void NvFlash_Read(uint32_t addr, void *out, uint32_t len)
{
  uint32_t at = addr - (BOARD_NV_PAGE_ADDR - 2 * PAGE);
  assert(at <= sizeof(flash) && len <= sizeof(flash)-at);
  memcpy(out, flash+at, len);
}
uint8_t NvFlash_WritePage(const void *bytes, uint32_t len)
{
  assert(len <= PAGE);
  memcpy(flash+2*PAGE, bytes, len);
  return 1;
}
uint8_t NvFlash_WriteCommissionPage(uint8_t slot, const void *bytes, uint32_t len)
{
  assert(!bridge && slot < 2 && len <= PAGE);
  if (fail_write) { return 0; }
  memcpy(flash+slot*PAGE, bytes, len);
  ++writes;
  return 1;
}
void Can_StopForFlash(void) {}
void Can_Restart(void) {}
uint8_t Can_Send(uint32_t id, const uint8_t d[8])
{
#if CFG_CAN_COMPAT
  assert(id == CFG_CAN_COMMISSION_BASE+1);
#else
  reply_id = id;
#endif
  memcpy(reply,d,8); return 1;
}
void Adc_RefreshVbus(void) {}
float Adc_GetVbusVolts(void) { return 24; }
uint8_t Adc_TempSensorPresent(void) { return 0; }
uint8_t Adc_GetTempRaw(uint16_t *out) { (void)out; return 0; }
uint8_t Adc_CurrentSenseReady(void) { return 1; }
uint8_t Adc_ReadPhaseCurrents(float *a,float *b) { *a = *b = 0; return 1; }
uint8_t Encoder_ReadRawFast(uint16_t *raw) { *raw = 0; return 1; }
uint8_t Encoder_ReadFrame(uint16_t *raw,uint8_t *status) { (void)status; return Encoder_ReadRawFast(raw); }
uint8_t Encoder2_ReadFrame(uint16_t *raw,uint8_t *status) { return Encoder_ReadFrame(raw,status); }
void Pwm_EnableOutputs(void) { bridge = 1; }
void Pwm_DisableOutputs(void) { bridge = 0; }
void Pwm_ApplyDuty(float a,float b,float c) { (void)a; (void)b; (void)c; }
void Pwm_ApplyDutyEarly(float a,float b,float c) { Pwm_ApplyDuty(a,b,c); }
void CtrlTimer_Start(void) {}

static uint32_t Bits(float x) { uint32_t bits; memcpy(&bits,&x,4); return bits; }
static uint32_t Crc(const uint32_t *words)
{
  uint32_t crc = UINT32_MAX;
  for (unsigned i=0; i<WORDS; ++i)
  {
    uint32_t word = words[i];
    for (unsigned bit=0; bit<32; ++bit)
    {
      uint32_t mix = (crc ^ word) & 1U;
      crc = (crc >> 1) ^ (mix ? UINT32_C(0xedb88320) : 0U);
      word >>= 1;
    }
  }
  return ~crc;
}

static void StageAndSave(const uint32_t *words,uint8_t expect_success)
{
  const uint8_t calibrated = Servo_GetMode() == SERVO_RUN;
  uint8_t cmd[8] = {0x37};
  Commission_Handle(cmd,1,calibrated);
  assert(reply[0] == 0x37);
  for (unsigned i=0; i<WORDS; ++i)
  {
    cmd[0]=0x36; cmd[2]=(uint8_t)i;
    memcpy(cmd+4, words+i, 4);
    Commission_Handle(cmd,1,calibrated);
    assert(reply[0] == 0x36);
  }
  cmd[0]=0x37; cmd[2]=1;
  uint32_t crc=Crc(words);
  memcpy(cmd+4,&crc,4);
  Commission_Handle(cmd,1,calibrated);
  assert(reply[0] == (expect_success ? 0x37 : 0xB7));
}

static uint8_t Boot(void)
{
  bridge = 0;
  Servo_Init();
  Commission_Init();
  for (unsigned i=0; i<CFG_ENC_STABLE_SAMPLES+1; ++i) { Servo_OnPwmIsr(); }
  return Cali_Start();
}
static void Near(float x,float expected) { assert(fabsf(x-expected) < 0.0001f); }
static float RunAtRest(void)
{
  ServoTelemetry_t t;
  for (unsigned i=0; i<64; ++i) { Servo_OnCtrlIsr(); Servo_OnPwmIsr(); }
  Servo_GetTelemetry(&t);
  assert(!Servo_GetFault());
  return t.iq_ref;
}
#if !CFG_CAN_COMPAT
static void CheckNativeWire(void)
{
  CanFrame_t torque = {.id=CFG_CAN_MOTION_BASE+1, .data={0,0,0,0,0,0,0xe8,0x03}};
  CanFrame_t iq = {.id=CFG_CAN_IQ_CMD_BASE+1, .data={0x0c,0xfe}}; /* -500 mA */
  CanFrame_t disable = {.id=CFG_CAN_MGMT_BASE+1, .data={2,10}};
  CanFrame_t config = {.id=CFG_CAN_MGMT_BASE+1, .data={0x22,11,2}};
  Comm_Init(1);
  Queue(config); Comm_Process();
  assert(reply_id == CFG_CAN_CURRENT_CONFIG_BASE+1 && reply[0] == 2 && reply[6] == 1 && reply[7] == 0);
  Queue(iq); Queue(torque); Comm_Process();
  Near(RunAtRest(),1.0f/(CFG_MOTOR_KT_NM_A*CFG_GEAR_RATIO)+0.2f);
  Queue(torque); Queue(iq); Comm_Process(); Near(RunAtRest(),-0.5f);
  iq.data[0]=iq.data[1]=0; iq.data[7]=1;
  Queue(iq); Comm_Process(); Near(RunAtRest(),-0.5f);
  iq.data[7]=0; iq.data[1]=0x7f;
  Queue(iq); Comm_Process(); Near(RunAtRest(),-0.5f);
  iq.data[1]=0;
  Queue(iq); Comm_Process(); Near(RunAtRest(),0); assert(bridge);
  Queue(torque); Queue(disable); Comm_Process(); Near(RunAtRest(),0); assert(!bridge);
  CanFrame_t mode = {.id=CFG_CAN_MGMT_BASE+1, .data={6,12,SERVO_CTRL_POSITION}};
  Queue(mode); Queue(iq); Comm_Process(); Near(RunAtRest(),0); assert(!bridge);
  mode.data[1]++; mode.data[2]=SERVO_CTRL_MOTION;
  Queue(mode); Comm_Process();
  Servo_SetInnerLoop(CFG_INNER_LOOP_VOLTAGE);
  Queue(iq); Comm_Process(); Near(RunAtRest(),0); assert(!bridge);
  Servo_SetInnerLoop(CFG_INNER_LOOP_CURRENT);
  Commission_ApplyControl();
}
#endif

static void CheckTuned(void)
{
  float p,i,d;
  Near(Commission_PhaseResistance(),3.0f);
  Near(Cfg_StallTorqueNm(24, Commission_PhaseResistance()),
       CFG_MOTOR_KT_NM_A * fminf(CFG_DRIVE_I_LIMIT_A, 3.6f));
  Servo_GetCurrentGains(&p,&i); Near(p,1.5f); Near(i,3000);
  Servo_GetVelocityGains(&p,&i); Near(p,0.02f); Near(i,0.2f);
  Servo_GetPositionGains(&p,&i,&d); Near(p,1); Near(i,0.1f); Near(d,0.01f);
}
static void CheckInvalidated(void)
{
  float p,i;
  Servo_GetCurrentGains(&p,&i);
  Near(p,CFG_MOTOR_L_H*(FOC_TWO_PI*CFG_CUR_BW_HZ));
  Near(i,CFG_MOTOR_R_OHM*(FOC_TWO_PI*CFG_CUR_BW_HZ));
  Servo_GetVelocityGains(&p,&i); Near(p,0); Near(i,0);
  Near(Commission_Cogging(0),0);
  Near(Commission_PhaseResistance(),4.0f);
}

int main(void)
{
  uint32_t words[WORDS]={0};
  memset(flash,0xff,sizeof(flash));
  assert(!Boot());
  Near(Commission_PhaseResistance(),CFG_MOTOR_R_OHM);
  words[0]=63; /* R/L, current PI, outer PID, both encoders, cogging */
  words[1]=Bits(3); words[2]=Bits(0.001f); words[3]=Bits(0.002f);
  words[4]=Bits(1.5f); words[5]=Bits(3000); words[6]=0x0001010e;
  words[8]=Bits(0.02f); words[9]=Bits(0.2f);
  words[10]=Bits(1); words[11]=Bits(0.1f); words[12]=Bits(0.01f);
  words[TABLE]=(300U<<16)|200U;
  words[TABLE+1]=(uint16_t)(int16_t)-100;
  StageAndSave(words,1);
  CheckTuned();
  assert(Boot() && !bridge && writes==1);
  CheckTuned();
  Near(RunAtRest(),0); assert(!bridge);
#if !CFG_CAN_COMPAT
  CheckNativeWire();
#endif
  Servo_SetGains(0,0); Servo_SetMotion(0,0,0); Near(RunAtRest(),0.2f);
  const float one_nm_amps = 1.0f / (CFG_MOTOR_KT_NM_A * CFG_GEAR_RATIO);
  Servo_SetMotion(0,0,Cfg_NmToPu(1)); Near(RunAtRest(),one_nm_amps+0.2f);
  Servo_SetClosedLoopDir(-1); Near(RunAtRest(),-one_nm_amps+0.2f);
  Servo_SetDirectIq(0.5f); Near(RunAtRest(),-0.5f);
  Servo_SetClosedLoopDir(1);
  Servo_SetDirectIq(0); Near(RunAtRest(),0);
  Servo_SetGains(0.1f,0.1f);
  Servo_SetMotion(4,0,0); Servo_SetDirectIq(-0.5f); Near(RunAtRest(),-0.5f);
  Servo_SetMotion(0,0,0);
  Servo_SetGains(0.1f,0); Near(RunAtRest(),0.2f);
  Servo_SetGains(0,0.1f); Near(RunAtRest(),0.2f);
  Servo_SetGains(0,0); Near(RunAtRest(),0.2f);
  Servo_SetCtrlMode(SERVO_CTRL_VELOCITY); Servo_SetVelocityCmd(0); Near(RunAtRest(),0.2f);
  Servo_SetCtrlMode(SERVO_CTRL_POSITION); Servo_SetPositionCmd(0,1); Near(RunAtRest(),0.2f);
  Near(Commission_Cogging(128),-0.10f);
  Near(Commission_Cogging(32),0.25f); /* half way between adjacent raw-angle bins */
#if CFG_CAN_COMPAT
  Servo_SetCtrlMode(SERVO_CTRL_MOTION); Servo_SetGains(0,0);
  Servo_SetMotion(0,0,Cfg_NmToPu(0.9f * CFG_MOTOR_KT_NM_A * CFG_GEAR_RATIO));
  Near(RunAtRest(),CFG_DRIVE_I_LIMIT_A);
  Servo_SetDirectIq(-10.0f); Near(RunAtRest(),-CFG_DRIVE_I_LIMIT_A);
#endif
  words[0]=1|8|16; words[1]=Bits(4); /* host saves a new R/L measurement */
  fail_write=1; StageAndSave(words,0); CheckTuned();
  fail_write=0; StageAndSave(words,1); CheckInvalidated();
  assert(Boot() && !bridge && writes==2); CheckInvalidated();
  Servo_SetGains(0,0); Servo_SetMotion(0,0,Cfg_NmToPu(1)); Near(RunAtRest(),one_nm_amps);
  flash[0]^=1; /* invalidate newest A/B slot; reload previous verified tune */
  assert(Boot() && !bridge && writes==2); CheckTuned();
  return 0;
}
