#include "comm.h"
#include "dm4310.h"

#include "adc.h"
#include "board.h"
#include "can.h"
#include "commission.h"
#include "config.h"
#include "debug.h"
#include "node.h"
#include "servo.h"

static uint8_t s_node;
static uint8_t s_calibrated;
static uint8_t s_enabled;
static uint8_t s_link_fault;
static uint32_t s_last_command_ms;

static void Disarm(void)
{
  if (s_enabled)
  {
    const ServoMode_t previous = Servo_PauseForFlash();
    Servo_ResumeAfterFlash(previous);
    s_enabled = 0U;
  }
}

void Comm_OnCanLoss(void)
{
  if (s_enabled) { s_link_fault = 1U; }
  Disarm();
}

static uint8_t Ready(void)
{
  return s_calibrated && Servo_GetMode() == SERVO_RUN && !Servo_GetFault() &&
         Servo_EncoderStable() && !(Servo_GetWarning() & SERVO_WARN_ABS_UNKNOWN) &&
         Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT && Adc_CurrentSenseReady() &&
         Adc_GetVbusVolts() >= CFG_VBUS_GAIN_MIN_V && !Commission_BlocksDrive();
}

static void CheckSession(void)
{
  if (!s_enabled) { return; }
  if (Can_RxLost() || Board_GetTickMs() - s_last_command_ms >= DM4310_TIMEOUT_MS)
  {
    Comm_OnCanLoss();
  }
  else if (!Ready()) { Disarm(); }
}

static uint8_t Status(void)
{
  const uint16_t fault = Servo_GetFault();
  if (fault & SERVO_FAULT_OVERCURRENT) { return 0xAU; }
  if (fault & SERVO_FAULT_UNDERVOLTAGE) { return 9U; }
  if (fault & SERVO_FAULT_OVER_TEMP) { return 0xBU; }
  if (s_link_fault) { return 0xDU; }
  /* Other JM1 faults have no equivalent DM code. Report disabled. */
  return s_enabled && !fault ? 1U : 0U;
}

static void SendFeedback(void)
{
  ServoTelemetry_t tel;
  float temp = 0.0f;
  uint8_t data[8];
  Servo_GetTelemetry(&tel);
  (void)Servo_GetTemperature(&temp);
  Dm_Feedback(data, s_node, Status(), tel.p_act, tel.v_act,
              tel.torque_nm * tel.closed_loop_dir * CFG_GEAR_RATIO, temp);
  (void)Can_Send(Dm_FeedbackId(s_node), data);
}

void Comm_Init(uint8_t cali_ok)
{
  s_node = Node_GetId();
  s_calibrated = cali_ok;
  s_enabled = 0U;
  s_link_fault = 0U;
  /* CAN started before calibration. Deinitialize the controller and software
   * queue so an enable sent during startup cannot arm the new session. */
  Can_StopForFlash();
  Can_Restart();
  Dbg_Printf("JM1 DM4310 MIT bench v1 node=%d mst=%d 1Mbps cal=%d id_ok=%d\r\n",
             (int)s_node, (int)Dm_FeedbackId(s_node), (int)cali_ok,
             (int)(s_node >= 1U && s_node <= DM4310_MAX_NODE));
}

uint8_t Comm_IsCalibrated(void) { return s_calibrated; }

void Comm_Process(void)
{
  CanFrame_t frame;
  DmMotion pending;
  uint8_t have_motion = 0U;
  CheckSession();
  if (s_node < 1U || s_node > DM4310_MAX_NODE || Can_RxLost()) { return; }

  /* Bound a batch to one RX ring. Stops/clears cancel any earlier target. */
  for (unsigned i = 0U; i < CFG_CAN_RX_SLOTS && Can_PopRx(&frame); ++i)
  {
    CheckSession();
    if (Can_RxLost()) { return; }
    if (!s_enabled) { have_motion = 0U; }
    if (frame.id == DM4310_REGISTER_ID)
    {
      uint8_t reply[8];
      if (Dm_ReadRegister(s_node, frame.data, reply)) { (void)Can_Send(Dm_FeedbackId(s_node), reply); }
      continue;
    }
    if (frame.id != s_node) { continue; }
    DmMotion motion;
    const DmCommand cmd = Dm_Decode(frame.data, &motion);
    if (cmd == DM_RESERVED) { continue; }
    if (cmd == DM_DISABLE || cmd == DM_ZERO || cmd == DM_CLEAR)
    {
      Disarm();
      have_motion = 0U;
      /* Persistent zero is serviced in the native maintenance image. */
      if (cmd == DM_ZERO) { continue; }
      if (cmd == DM_CLEAR)
      {
        if (Board_GetTickMs() - frame.received_ms >= DM4310_TIMEOUT_MS) { continue; }
        s_link_fault = 0U;
        if (s_calibrated && Servo_GetFault() && Servo_ClearFault())
        { Servo_ResumeAfterFlash(SERVO_RUN); }
      }
    }
    else if (Board_GetTickMs() - frame.received_ms >= DM4310_TIMEOUT_MS) { continue; }
    else if (cmd == DM_ENABLE)
    {
      if (!s_enabled && Ready())
      {
        s_enabled = 1U;
        s_link_fault = 0U;
        s_last_command_ms = frame.received_ms;
      }
    }
    else if (s_enabled)
    {
      pending = motion;
      have_motion = 1U;
      s_last_command_ms = frame.received_ms;
    }
    SendFeedback();
  }

  CheckSession();
  if (have_motion && s_enabled)
  {
    const float kp = Cfg_NmToPu(pending.kp);
    const float kd = Cfg_NmToPu(pending.kd);
    const float ff = Cfg_NmToPu(pending.torque);
    const uint32_t lock = Board_IrqLock();
    /* A CAN error or servo trip may have preempted decoding. Publish both
     * halves before the outer-loop ISR can observe the new gains. */
    if (!Can_RxLost() && Ready() &&
        Board_GetTickMs() - s_last_command_ms < DM4310_TIMEOUT_MS)
    {
      Servo_SetGains(kp, kd);
      Servo_SetMotion(pending.position, pending.velocity, ff);
    }
    else { Disarm(); }
    Board_IrqUnlock(lock);
  }
}
