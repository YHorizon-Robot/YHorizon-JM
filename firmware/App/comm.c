#include "comm.h"

#include "adc.h"
#include "cali.h"
#include "cali_nv.h"
#include "can.h"
#include "config.h"
#include "foc_math.h"
#include "node.h"
#include "pwm.h"
#include "servo.h"

#define COMM_MODE_MOTION            (0x00U)
#define COMM_MODE_VELOCITY          (0x01U)
#define COMM_MODE_POSITION          (0x02U)

#define COMM_STATE_RUNNING          (0x02U)
#define COMM_STATE_FAULT            (0x03U)
#define COMM_STATE_CALIBRATING      (0x04U)

#define COMM_CMD_SET_ZERO           (0x03U)
#define COMM_CMD_CLEAR_FAULT        (0x04U)
#define COMM_CMD_START_CALI         (0x05U)
#define COMM_CMD_SET_MODE           (0x06U)
#define COMM_CMD_SET_NODE_ID        (0x07U)
#define COMM_CMD_SAVE_USER_PARAMS   (0x08U)
#define COMM_CMD_GET_STATUS         (0x10U)
#define COMM_CMD_SET_GAINS          (0x20U)
#define COMM_CMD_GET_GAINS          (0x21U)

#define COMM_RES_OK                 (0x00U)
#define COMM_RES_INVALID_COMMAND    (0x01U)
#define COMM_RES_INVALID_STATE      (0x02U)
#define COMM_RES_OUT_OF_RANGE       (0x03U)
#define COMM_RES_FAULT_ACTIVE       (0x04U)
#define COMM_RES_BUSY               (0x05U)
#define COMM_RES_INTERNAL_ERROR     (0x06U)

#define COMM_FAULT_CALIBRATION      (1U << 10)

static uint8_t s_state;
static uint8_t s_ctrl_mode;
static uint16_t s_fault;
static uint8_t s_last_cmd;
static uint8_t s_last_seq;
static uint8_t s_have_cache;
static uint32_t s_cache_id;
static uint8_t s_cache_data[8];
static uint8_t s_drop_pending_rt;
static uint8_t s_mode_cleared_rt;

static uint32_t Comm_ReadU32Le(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t Comm_ReadU16Le(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t Comm_ReadI32Le(const uint8_t *p)
{
  return (int32_t)((uint32_t)p[0] |
                   ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24));
}

static void Comm_WriteU16Le(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)(v >> 8);
}

static void Comm_WriteI16Le(uint8_t *p, int16_t v)
{
  Comm_WriteU16Le(p, (uint16_t)v);
}

static void Comm_WriteI32Le(uint8_t *p, int32_t v)
{
  const uint32_t u = (uint32_t)v;
  p[0] = (uint8_t)(u & 0xFFU);
  p[1] = (uint8_t)((u >> 8) & 0xFFU);
  p[2] = (uint8_t)((u >> 16) & 0xFFU);
  p[3] = (uint8_t)((u >> 24) & 0xFFU);
}

static int32_t Comm_FloatToI32(float x, float lsb)
{
  const float scaled = x / lsb;
  return (int32_t)((scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f));
}

static int16_t Comm_FloatToI16(float x, float lsb)
{
  const int32_t v = Comm_FloatToI32(x, lsb);
  if (v > 32767)
  {
    return (int16_t)32767;
  }
  if (v < -32768)
  {
    return (int16_t)-32768;
  }
  return (int16_t)v;
}

static uint16_t Comm_FloatToU16(float x, float lsb)
{
  const int32_t v = Comm_FloatToI32(x, lsb);
  if (v < 0)
  {
    return 0U;
  }
  if (v > 65535)
  {
    return 65535U;
  }
  return (uint16_t)v;
}

static uint8_t Comm_ReservedZero(const uint8_t *p, uint8_t offset)
{
  uint8_t i;
  for (i = offset; i < 8U; i++)
  {
    if (p[i] != 0U)
    {
      return 0U;
    }
  }
  return 1U;
}

static void Comm_SendRaw(uint32_t id, const uint8_t data[8])
{
  (void)Can_Send(id, data);
}

static void Comm_FlushRx(void)
{
  CanFrame_t dump;

  while (Can_PopRx(&dump) != 0U)
  {
  }
}

static void Comm_CacheSend(uint32_t id, uint8_t cmd, uint8_t seq, const uint8_t data[8])
{
  uint8_t i;
  s_last_cmd = cmd;
  s_last_seq = seq;
  s_have_cache = 1U;
  s_cache_id = id;
  for (i = 0U; i < 8U; i++)
  {
    s_cache_data[i] = data[i];
  }
  Comm_SendRaw(id, data);
}

static uint8_t Comm_ReplayIfDup(uint8_t cmd, uint8_t seq)
{
  if ((s_have_cache != 0U) && (s_last_cmd == cmd) && (s_last_seq == seq))
  {
    Comm_SendRaw(s_cache_id, s_cache_data);
    return 1U;
  }
  return 0U;
}

static uint32_t Comm_Nid(void)
{
  return (uint32_t)Node_GetId();
}

static void Comm_SendAckArg(uint8_t cmd, uint8_t seq, uint8_t result, uint8_t arg)
{
  uint8_t data[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  data[0] = cmd;
  data[1] = seq;
  data[2] = result;
  data[3] = s_state;
  Comm_WriteU16Le(&data[4], s_fault);
  data[6] = arg;
  Comm_CacheSend(CFG_CAN_ACK_BASE + Comm_Nid(), cmd, seq, data);
}

static void Comm_SendAck(uint8_t cmd, uint8_t seq, uint8_t result)
{
  Comm_SendAckArg(cmd, seq, result, 0U);
}

static void Comm_SendStatus(uint8_t seq)
{
  uint8_t data[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  int32_t vbus_raw;

  data[0] = s_state;
  Comm_WriteU16Le(&data[1], s_fault);
  vbus_raw = Comm_FloatToI32(Adc_GetVbusVolts(), CFG_VBUS_LSB);
  if (vbus_raw < 0)
  {
    vbus_raw = 0;
  }
  if (vbus_raw > 65535)
  {
    vbus_raw = 65535;
  }
  Comm_WriteU16Le(&data[3], (uint16_t)vbus_raw);
  data[7] = (uint8_t)(((Servo_GetCtrlMode() & 0x03U) << 5)
                      | ((Servo_GetInnerLoop() == CFG_INNER_LOOP_CURRENT) ? 0x80U : 0x00U));
  Comm_CacheSend(CFG_CAN_STATUS_BASE + Comm_Nid(), COMM_CMD_GET_STATUS, seq, data);
}

static void Comm_SendFeedback(void)
{
  ServoTelemetry_t tel;
  uint8_t data[8];

  Servo_GetTelemetry(&tel);
  Comm_WriteI32Le(&data[0], Comm_FloatToI32(tel.p_act, CFG_POS_LSB));
  Comm_WriteI16Le(&data[4], Comm_FloatToI16(tel.v_act, CFG_VEL_LSB));
  /** t_ref, 0.001 pu/LSB. Voltage loop: Vq. Current loop: Iq_ref = t_ref * I_LIMIT / V_LIMIT. */
  Comm_WriteI16Le(&data[6], Comm_FloatToI16(tel.t_ref, CFG_TORQUE_LSB));
  Comm_SendRaw(CFG_CAN_FB_BASE + Comm_Nid(), data);
}

static uint8_t Comm_ParseMotion(const uint8_t *d, float *p, float *v, float *ff)
{
  const float vel = (float)((int16_t)Comm_ReadU16Le(&d[4])) * CFG_VEL_LSB;

  *p = (float)Comm_ReadI32Le(&d[0]) * CFG_POS_LSB;
  *v = Foc_Clamp(vel, CFG_VEL_CMD_MIN, CFG_VEL_CMD_MAX);
  *ff = Foc_Clamp((float)((int16_t)Comm_ReadU16Le(&d[6])) / 32767.0f, -1.0f, 1.0f);
  return 1U;
}

static uint8_t Comm_ParseVelocity(const uint8_t *d, float *v)
{
  const float vel = (float)Comm_ReadI32Le(&d[0]) * CFG_VEL_LSB;

  if (Comm_ReservedZero(d, 4U) == 0U)
  {
    return 0U;
  }
  *v = Foc_Clamp(vel, CFG_VEL_CMD_MIN, CFG_VEL_CMD_MAX);
  return 1U;
}

static uint8_t Comm_ParsePosition(const uint8_t *d, float *p, float *vmax)
{
  const float max_vel = (float)Comm_ReadU32Le(&d[4]) * CFG_VEL_LSB;

  *p = (float)Comm_ReadI32Le(&d[0]) * CFG_POS_LSB;
  *vmax = Foc_Clamp(max_vel, 0.0f, CFG_VEL_CMD_MAX);
  return 1U;
}

static void Comm_HandleGains(const uint8_t *d)
{
  const uint8_t mode = d[0];
  const uint8_t seq = d[1];
  const uint16_t kp_raw = Comm_ReadU16Le(&d[2]);
  const uint16_t ki_raw = Comm_ReadU16Le(&d[4]);
  const uint16_t kd_raw = Comm_ReadU16Le(&d[6]);
  float kp;
  float ki;
  float kd;

  if (Comm_ReplayIfDup(COMM_CMD_SET_GAINS, seq) != 0U)
  {
    return;
  }
  if (mode > COMM_MODE_POSITION)
  {
    Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
    return;
  }

  if (mode == COMM_MODE_MOTION)
  {
    if (ki_raw != 0U)
    {
      Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
      return;
    }
    kp = (float)kp_raw * CFG_KP_LSB;
    kd = (float)kd_raw * CFG_KD_LSB;
    if ((kp < CFG_KP_MIN) || (kp > CFG_KP_MAX) || (kd < CFG_KD_MIN) || (kd > CFG_KD_MAX))
    {
      Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
      return;
    }
    Servo_SetGains(kp, kd);
  }
  else if (mode == COMM_MODE_VELOCITY)
  {
    if (kd_raw != 0U)
    {
      Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
      return;
    }
    kp = (float)kp_raw * CFG_KP_VEL_LSB;
    ki = (float)ki_raw * CFG_KI_VEL_LSB;
    if ((kp > CFG_KP_VEL_MAX) || (ki > CFG_KI_VEL_MAX))
    {
      Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
      return;
    }
    Servo_SetVelocityGains(kp, ki);
  }
  else
  {
    kp = (float)kp_raw * CFG_KP_POS_LSB;
    ki = (float)ki_raw * CFG_KI_POS_LSB;
    kd = (float)kd_raw * CFG_KD_POS_LSB;
    if ((kp > CFG_KP_POS_MAX) || (ki > CFG_KI_POS_MAX) || (kd > CFG_KD_POS_MAX))
    {
      Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
      return;
    }
    Servo_SetPositionGains(kp, ki, kd);
  }
  Comm_SendAck(COMM_CMD_SET_GAINS, seq, COMM_RES_OK);
}

static void Comm_SendGainsReply(uint8_t seq, uint8_t mode,
                                float kp, float ki, float kd,
                                float kp_lsb, float ki_lsb, float kd_lsb)
{
  uint8_t data[8];

  data[0] = mode;
  data[1] = seq;
  Comm_WriteU16Le(&data[2], Comm_FloatToU16(kp, kp_lsb));
  Comm_WriteU16Le(&data[4], Comm_FloatToU16(ki, ki_lsb));
  Comm_WriteU16Le(&data[6], Comm_FloatToU16(kd, kd_lsb));
  Comm_CacheSend(CFG_CAN_GAINS_BASE + Comm_Nid(), COMM_CMD_GET_GAINS, seq, data);
}

static void Comm_HandleGetGains(const uint8_t *d)
{
  const uint8_t seq = d[1];
  const uint8_t mode = d[2];
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;

  if (Comm_ReplayIfDup(COMM_CMD_GET_GAINS, seq) != 0U)
  {
    return;
  }
  if ((mode > COMM_MODE_POSITION) || (Comm_ReservedZero(d, 3U) == 0U))
  {
    Comm_SendAck(COMM_CMD_GET_GAINS, seq, COMM_RES_OUT_OF_RANGE);
    return;
  }

  if (mode == COMM_MODE_MOTION)
  {
    Servo_GetGains(&kp, &kd);
    Comm_SendGainsReply(seq, mode, kp, 0.0f, kd, CFG_KP_LSB, CFG_KP_LSB, CFG_KD_LSB);
  }
  else if (mode == COMM_MODE_VELOCITY)
  {
    Servo_GetVelocityGains(&kp, &ki);
    Comm_SendGainsReply(seq, mode, kp, ki, 0.0f, CFG_KP_VEL_LSB, CFG_KI_VEL_LSB, CFG_KD_LSB);
  }
  else
  {
    Servo_GetPositionGains(&kp, &ki, &kd);
    Comm_SendGainsReply(seq, mode, kp, ki, kd, CFG_KP_POS_LSB, CFG_KI_POS_LSB, CFG_KD_POS_LSB);
  }
}

static void Comm_HandleSaveUserParams(uint8_t seq)
{
  float vel_kp = 0.0f;
  float vel_ki = 0.0f;
  float pos_kp = 0.0f;
  float pos_ki = 0.0f;
  float pos_kd = 0.0f;
  uint8_t ok;

  if (s_state == COMM_STATE_CALIBRATING)
  {
    Comm_SendAck(COMM_CMD_SAVE_USER_PARAMS, seq, COMM_RES_BUSY);
    return;
  }

  Servo_GetVelocityGains(&vel_kp, &vel_ki);
  Servo_GetPositionGains(&pos_kp, &pos_ki, &pos_kd);
  Servo_HoldPosition();
  Can_StopForFlash();
  ok = CaliNv_SaveUserGains(Node_GetId(),
                            Comm_FloatToU16(vel_kp, CFG_KP_VEL_LSB),
                            Comm_FloatToU16(vel_ki, CFG_KI_VEL_LSB),
                            Comm_FloatToU16(pos_kp, CFG_KP_POS_LSB),
                            Comm_FloatToU16(pos_ki, CFG_KI_POS_LSB),
                            Comm_FloatToU16(pos_kd, CFG_KD_POS_LSB));
  Can_Restart();
  Comm_SendAck(COMM_CMD_SAVE_USER_PARAMS, seq,
               (ok != 0U) ? COMM_RES_OK : COMM_RES_INTERNAL_ERROR);
}

static void Comm_HandleMgmt(const uint8_t *d)
{
  const uint8_t cmd = d[0];
  const uint8_t seq = d[1];

  if (Comm_ReplayIfDup(cmd, seq) != 0U)
  {
    return;
  }

  switch (cmd)
  {
    case COMM_CMD_SET_ZERO:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      if (s_state == COMM_STATE_FAULT)
      {
        Comm_SendAck(cmd, seq, COMM_RES_FAULT_ACTIVE);
        break;
      }
      Servo_SetZero();
      {
        uint8_t ok;
        Can_StopForFlash();
        ok = CaliNv_SaveMtZero(Servo_GetZeroMotor());
        Can_Restart();
        Comm_SendAck(cmd, seq, (ok != 0U) ? COMM_RES_OK : COMM_RES_INTERNAL_ERROR);
      }
      break;

    case COMM_CMD_CLEAR_FAULT:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      if (s_state == COMM_STATE_FAULT)
      {
        if (s_fault != COMM_FAULT_CALIBRATION)
        {
          Comm_SendAck(cmd, seq, COMM_RES_FAULT_ACTIVE);
          break;
        }
        s_fault = 0U;
        s_state = COMM_STATE_RUNNING;
        Servo_SetMode(SERVO_IDLE);
        Pwm_DisableOutputs();
      }
      Comm_SendAck(cmd, seq, COMM_RES_OK);
      break;

    case COMM_CMD_SET_MODE:
      if ((d[2] > COMM_MODE_POSITION) || (Comm_ReservedZero(d, 3U) == 0U))
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      if (s_state == COMM_STATE_FAULT)
      {
        Comm_SendAck(cmd, seq, COMM_RES_INVALID_STATE);
        break;
      }
      if (s_state == COMM_STATE_CALIBRATING)
      {
        Comm_SendAck(cmd, seq, COMM_RES_BUSY);
        break;
      }
      s_ctrl_mode = d[2];
      Servo_SetCtrlMode(d[2]);
      s_mode_cleared_rt = 1U;
      Comm_SendAck(cmd, seq, COMM_RES_OK);
      break;

    case COMM_CMD_SET_NODE_ID:
      if ((d[2] < CFG_NODE_ID_MIN) || (d[2] > CFG_NODE_ID_MAX) ||
          (Comm_ReservedZero(d, 3U) == 0U))
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      if (s_state == COMM_STATE_CALIBRATING)
      {
        Comm_SendAck(cmd, seq, COMM_RES_BUSY);
        break;
      }
      Servo_HoldPosition();
      if (Node_ApplyAndReset(d[2]) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_INTERNAL_ERROR);
        break;
      }
      Comm_SendAckArg(cmd, seq, COMM_RES_OK, d[2]);
      break;

    case COMM_CMD_SAVE_USER_PARAMS:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_HandleSaveUserParams(seq);
      break;

    case COMM_CMD_GET_GAINS:
      Comm_HandleGetGains(d);
      break;

    case COMM_CMD_GET_STATUS:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_SendStatus(seq);
      break;

    case COMM_CMD_START_CALI:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      if (s_state == COMM_STATE_FAULT)
      {
        Comm_SendAck(cmd, seq, COMM_RES_FAULT_ACTIVE);
        break;
      }
      if (s_state == COMM_STATE_CALIBRATING)
      {
        Comm_SendAck(cmd, seq, COMM_RES_BUSY);
        break;
      }
      s_state = COMM_STATE_CALIBRATING;
      Comm_SendAck(cmd, seq, COMM_RES_OK);
      if (Cali_RunCommand(seq) != 0U)
      {
        s_state = COMM_STATE_RUNNING;
        s_fault = 0U;
      }
      else
      {
        s_state = COMM_STATE_FAULT;
        s_fault = COMM_FAULT_CALIBRATION;
      }
      Comm_FlushRx();
      s_drop_pending_rt = 1U;
      break;

    default:
      Comm_SendAck(cmd, seq, COMM_RES_INVALID_COMMAND);
      break;
  }
}

void Comm_Init(uint8_t cali_ok)
{
  s_ctrl_mode = COMM_MODE_MOTION;
  s_have_cache = 0U;
  s_drop_pending_rt = 0U;
  s_mode_cleared_rt = 0U;
  if (cali_ok != 0U)
  {
    s_state = COMM_STATE_RUNNING;
    s_fault = 0U;
  }
  else
  {
    s_state = COMM_STATE_FAULT;
    s_fault = COMM_FAULT_CALIBRATION;
  }
}

void Comm_Process(void)
{
  CanFrame_t frame;
  uint8_t have_rt = 0U;
  float p_set = 0.0f;
  float v_set = 0.0f;
  float t_ff = 0.0f;
  float v_max = 0.0f;
  const uint32_t nid = Comm_Nid();

  while (Can_PopRx(&frame) != 0U)
  {
    if (frame.id == (CFG_CAN_MOTION_BASE + nid))
    {
      float p;
      float v;
      float ff;
      if ((s_ctrl_mode == COMM_MODE_MOTION) && (Comm_ParseMotion(frame.data, &p, &v, &ff) != 0U))
      {
        p_set = p;
        v_set = v;
        t_ff = ff;
        have_rt = COMM_MODE_MOTION + 1U;
      }
    }
    else if (frame.id == (CFG_CAN_VEL_BASE + nid))
    {
      float v;
      if ((s_ctrl_mode == COMM_MODE_VELOCITY) && (Comm_ParseVelocity(frame.data, &v) != 0U))
      {
        v_set = v;
        have_rt = COMM_MODE_VELOCITY + 1U;
      }
    }
    else if (frame.id == (CFG_CAN_POS_BASE + nid))
    {
      float p;
      float vmax;
      if ((s_ctrl_mode == COMM_MODE_POSITION) && (Comm_ParsePosition(frame.data, &p, &vmax) != 0U))
      {
        p_set = p;
        v_max = vmax;
        have_rt = COMM_MODE_POSITION + 1U;
      }
    }
    else if (frame.id == (CFG_CAN_GAINS_BASE + nid))
    {
      Comm_HandleGains(frame.data);
    }
    else if (frame.id == (CFG_CAN_MGMT_BASE + nid))
    {
      Comm_HandleMgmt(frame.data);
      if (s_mode_cleared_rt != 0U)
      {
        s_mode_cleared_rt = 0U;
        have_rt = 0U;
      }
    }
  }

  if (s_drop_pending_rt != 0U)
  {
    s_drop_pending_rt = 0U;
    have_rt = 0U;
  }
  if (have_rt == 0U)
  {
    return;
  }
  if (s_state != COMM_STATE_RUNNING)
  {
    return;
  }
  if (Servo_GetMode() != SERVO_RUN)
  {
    return;
  }

  if (have_rt == (COMM_MODE_MOTION + 1U))
  {
    Servo_SetMotion(p_set, v_set, t_ff);
  }
  else if (have_rt == (COMM_MODE_VELOCITY + 1U))
  {
    Servo_SetVelocityCmd(v_set);
  }
  else
  {
    Servo_SetPositionCmd(p_set, v_max);
  }
  Comm_SendFeedback();
}
