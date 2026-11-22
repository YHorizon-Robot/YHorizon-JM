#include "profile.h"
#include "comm.h"
#include "adc.h"
#include "board.h"
#include "commission.h"
#include "config.h"
#include "debug.h"
#include "node.h"
#include "servo.h"

static uint8_t s_node, s_calibrated, s_enabled, s_driving;
static uint32_t s_last_command;
#if CFG_RS05 || CFG_CYBERGEAR
static float s_zero_offset;
#endif
#if CFG_C610 || CFG_C620
static uint32_t s_feedback_tick;
#endif

static uint8_t Ready(void)
{
  return s_calibrated && Servo_GetMode() == SERVO_RUN && !Servo_GetFault() &&
         Servo_EncoderStable() && !(Servo_GetWarning() & SERVO_WARN_ABS_UNKNOWN) &&
         Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT && Adc_CurrentSenseReady() &&
         Adc_GetVbusVolts() >= CFG_VBUS_GAIN_MIN_V && !Commission_BlocksDrive();
}

void Comm_OnCanLoss(void)
{
  if (s_enabled)
  {
    ServoMode_t previous = Servo_PauseForFlash();
    Servo_ResumeAfterFlash(previous);
  }
  s_driving = s_enabled = 0;
}

static uint8_t Expired(uint32_t arrival)
{
  return Board_GetTickMs() - arrival >= PROFILE_TIMEOUT_MS;
}

static void CheckSession(void)
{
  if (s_enabled && (Expired(s_last_command) || Can_RxLost() || !Ready())) { Comm_OnCanLoss(); }
}

static void Feedback(const CanFrame_t *request)
{
  ProfileState state = {0};
  CanFrame_t response = {0};
  ServoTelemetry_t tel;
  Servo_GetTelemetry(&tel);
  state.position = tel.p_act;
#if CFG_RS05 || CFG_CYBERGEAR
  state.position -= s_zero_offset;
  if ((request->id >> 24) == 0U) { Board_GetUid64(state.uid); }
#endif
  state.velocity = tel.v_act;
  state.torque = tel.torque_nm * tel.closed_loop_dir * CFG_GEAR_RATIO;
  state.amps = tel.iq * tel.closed_loop_dir;
#if CFG_C610 || CFG_C620
  state.rotor_angle = Foc_EncoderGetMechWrapped(Servo_GetEncoder());
#endif
  (void)Servo_GetTemperature(&state.temperature);
  state.vbus = Adc_GetVbusVolts();
  state.fault = Servo_GetFault();
  state.enabled = s_enabled;
  state.calibrated = s_calibrated;
  if (ProfileReply(s_node, request, &state, &response)) { (void)Can_SendFrame(&response); }
}

void Comm_Init(uint8_t cali_ok)
{
  Comm_OnCanLoss();
  s_node = Node_GetId();
  s_calibrated = cali_ok;
#if CFG_RS05 || CFG_CYBERGEAR
  s_zero_offset = 0;
#endif
#if CFG_C610 || CFG_C620
  s_feedback_tick = Board_GetTickMs();
#endif
  Can_StopForFlash();
  Can_Restart();
  Dbg_Printf("JM1 %s bench node=%d 1Mbps cal=%d id_ok=%d\r\n",
             JM1_PROFILE_NAME, (int)s_node, (int)cali_ok, (int)ProfileNodeValid(s_node));
}

uint8_t Comm_IsCalibrated(void) { return s_calibrated; }

#if CFG_RMD_X || CFG_LK_MF
static uint8_t OuterLoopReady(ProfileAction action)
{
  float kp, ki;
  Servo_GetVelocityGains(&kp, &ki);
  if (!(kp > 0 || ki > 0)) { return 0; }
  if (action == CMD_POSITION)
  {
    Servo_GetPositionGains(&kp, &ki, NULL);
    return kp > 0 || ki > 0;
  }
  return 1;
}
#endif

static void Apply(ProfileAction action, const ProfileTarget *target)
{
  uint8_t mode = SERVO_CTRL_MOTION;
  if (action == CMD_VELOCITY) { mode = SERVO_CTRL_VELOCITY; }
  if (action == CMD_POSITION) { mode = SERVO_CTRL_POSITION; }
  if (Servo_GetCtrlMode() != mode) { Servo_SetCtrlMode(mode); }
  if (action == CMD_VELOCITY) { Servo_SetVelocityCmd(target->velocity); }
  else if (action == CMD_POSITION) { Servo_SetPositionCmd(target->position, target->velocity); }
  else
  {
    float ff = action == CMD_CURRENT ? target->amps * (CFG_V_LIMIT / CFG_I_LIMIT_A)
                                     : Cfg_NmToPu(target->torque);
    float position = target->position;
#if CFG_RS05 || CFG_CYBERGEAR
    position += s_zero_offset;
#endif
    Servo_SetGains(Cfg_NmToPu(target->kp), Cfg_NmToPu(target->kd));
    Servo_SetMotion(position, target->velocity, ff);
  }
  s_driving = 1;
}

void Comm_Process(void)
{
  CanFrame_t frame;
  ProfileAction pending = CMD_IGNORE;
  ProfileTarget target = {0};
  CheckSession();
  if (!ProfileNodeValid(s_node) || Can_RxLost()) { return; }
  for (unsigned i = 0; i < CFG_CAN_RX_SLOTS && Can_PopRx(&frame); ++i)
  {
    CheckSession();
    if (Can_RxLost()) { return; }
    if (!s_enabled) { pending = CMD_IGNORE; }
    if (!ProfileAccepts(s_node, frame.id, frame.extended, frame.dlc)) { continue; }
    ProfileTarget next = {0};
    ProfileAction action = ProfileDecode(s_node, &frame, &next);
    if (action == CMD_IGNORE) { continue; }
    if (action == CMD_STOP || action == CMD_CLEAR)
    {
      Comm_OnCanLoss();
      pending = CMD_IGNORE;
      if (action == CMD_CLEAR && !Expired(frame.received_ms) && s_calibrated &&
          Servo_GetFault() && Servo_ClearFault()) { Servo_ResumeAfterFlash(SERVO_RUN); }
    }
    else if (Expired(frame.received_ms)) { continue; }
    else if (action == CMD_ENABLE || action == CMD_NEUTRAL)
    {
      if (action == CMD_NEUTRAL)
      {
        pending = CMD_IGNORE;
        if (s_driving) { Comm_OnCanLoss(); }
      }
      if ((!s_enabled || action == CMD_NEUTRAL) && Ready())
      { s_enabled = 1; s_last_command = frame.received_ms; }
    }
#if CFG_RS05 || CFG_CYBERGEAR
    else if (action == CMD_RAM_ZERO)
    {
      if (s_enabled || !Ready()) { continue; }
      ServoTelemetry_t tel;
      Servo_GetTelemetry(&tel);
      if (!isfinite(tel.p_act) || !(fabsf(tel.v_act) <= 0.05f)) { continue; }
      s_zero_offset = tel.p_act;
    }
#endif
    else if (action != CMD_READ && s_enabled)
    {
#if CFG_RMD_X || CFG_LK_MF
      if ((action == CMD_POSITION || action == CMD_VELOCITY) && !OuterLoopReady(action))
      {
        Comm_OnCanLoss();
        pending = CMD_IGNORE;
        continue;
      }
#endif
      target = next;
      pending = action;
      s_last_command = frame.received_ms;
    }
#if !CFG_C610 && !CFG_C620
    Feedback(&frame);
#endif
  }
  if (pending != CMD_IGNORE && s_enabled)
  {
    const uint32_t lock = Board_IrqLock();
    if (!Can_RxLost() && !Expired(s_last_command) && Ready()) { Apply(pending, &target); }
    else { Comm_OnCanLoss(); }
    Board_IrqUnlock(lock);
  }
  CheckSession();
#if CFG_C610 || CFG_C620
  const uint32_t now = Board_GetTickMs();
  if (now != s_feedback_tick && !Can_RxLost()) { s_feedback_tick = now; Feedback(NULL); }
#endif
}
