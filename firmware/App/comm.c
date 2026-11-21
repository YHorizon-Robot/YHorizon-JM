#include "comm.h"

#include "adc.h"
#include "cali_nv.h"
#include "can.h"
#include "multi_axis.h"
#include "config.h"
#if (CFG_COMMISSION != 0U)
#include "commission.h"
#endif
#include "foc_math.h"
#include "node.h"
#include "pwm.h"
#include "servo.h"

#define COMM_MODE_MOTION            (0x00U)
#define COMM_MODE_VELOCITY          (0x01U)
#define COMM_MODE_POSITION          (0x02U)

#define COMM_STATE_RUNNING          (0x02U)
#define COMM_STATE_FAULT            (0x03U)

#define COMM_CMD_DISABLE            (0x02U)
#define COMM_CMD_SET_ZERO           (0x03U)
#define COMM_CMD_CLEAR_FAULT        (0x04U)
#define COMM_CMD_SET_MODE           (0x06U)
#define COMM_CMD_SET_NODE_ID        (0x07U)
#define COMM_CMD_SAVE_USER_PARAMS   (0x08U)
#define COMM_CMD_GET_DEVICE_INFO    (0x09U)
#define COMM_CMD_SET_INNER_LOOP     (0x0AU)
#define COMM_CMD_GET_STATUS         (0x10U)
#define COMM_CMD_GET_FEEDBACK       (0x11U)
#define COMM_CMD_SET_GAINS          (0x20U)
#define COMM_CMD_GET_GAINS          (0x21U)
#define COMM_CMD_GET_CURRENT_CONFIG (0x22U)
#define COMM_CMD_GET_CURRENT_STATE  (0x23U)

#define COMM_RES_OK                 (0x00U)
#define COMM_RES_INVALID_COMMAND    (0x01U)
#define COMM_RES_INVALID_STATE      (0x02U)
#define COMM_RES_OUT_OF_RANGE       (0x03U)
#define COMM_RES_FAULT_ACTIVE       (0x04U)
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
static uint8_t s_calibrated;
static uint8_t s_fault_event;
static uint8_t s_warning_seen;

/**
 * @brief 同步伺服层故障和警告。出现新的故障位或警告位时置位事件，由 Comm_Process 主动上报一帧状态。
 */
static void Comm_SyncFault(void)
{
  const uint16_t fault = Servo_GetFault();
  const uint8_t warning = Servo_GetWarning();

  /** 警告位新增时同样主动上报一帧状态。 */
  if ((uint8_t)(warning & (uint8_t)~s_warning_seen) != 0U)
  {
    s_fault_event = 1U;
  }
  s_warning_seen = warning;
  if (fault != 0U)
  {
    if ((uint16_t)(fault & (uint16_t)~s_fault) != 0U)
    {
      s_fault_event = 1U;
    }
    s_fault |= fault;
    s_state = COMM_STATE_FAULT;
  }
}

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

/**
 * @brief  GET_DEVICE_INFO 成功时 Byte4..7 是板卡号、能力、固件主次版本，不再放故障码。
 */
static void Comm_SendDeviceInfo(uint8_t seq)
{
  uint8_t data[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  uint8_t caps = 0U;

#if (BOARD_HAS_ENCODER2 != 0U)
  caps |= 0x01U;
#endif
#if (BOARD_HAS_CURRENT_SENSE != 0U)
  caps |= 0x02U;
#endif
#if (BOARD_CURRENT_CONTROL_SUPPORTED != 0U)
  caps |= 0x08U;
#endif
#if (CFG_FACTORY_TEST != 0U)
  caps |= 0x04U;
#endif
  /** @brief bit4：支持 DISABLE、广播停止、GET_FEEDBACK 与故障主动上报。 */
  caps |= 0x10U;
  /** @brief bit5：状态帧 Byte7 的 bit0 是板温警告，bit4 是多圈位置未建立（替代原协议的位置限位警告）。 */
  caps |= 0x20U;
  /** @brief bit6：上电探测到 NTC，状态帧的温度字段有效。 */
  if (Adc_TempSensorPresent() != 0U)
  {
    caps |= 0x40U;
  }
#if (CFG_COMMISSION != 0U)
  caps |= 0x80U;
#endif
  data[0] = COMM_CMD_GET_DEVICE_INFO;
  data[1] = seq;
  data[2] = COMM_RES_OK;
  data[3] = s_state;
  data[4] = (uint8_t)BOARD_ID;
  data[5] = caps;
  data[6] = (uint8_t)CFG_FW_MAJOR;
  data[7] = (uint8_t)CFG_FW_MINOR;
  Comm_CacheSend(CFG_CAN_ACK_BASE + Comm_Nid(), COMM_CMD_GET_DEVICE_INFO, seq, data);
}

static void Comm_BuildStatus(uint8_t data[8])
{
  int32_t vbus_raw;
  float temp_c = 0.0f;
  uint8_t i;

  for (i = 0U; i < 8U; i++)
  {
    data[i] = 0U;
  }
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
  if (Servo_GetTemperature(&temp_c) != 0U)
  {
    Comm_WriteI16Le(&data[5], Comm_FloatToI16(temp_c, 0.1f));
  }
  data[7] = (uint8_t)(Servo_GetWarning() & 0x1FU);
  data[7] |= (uint8_t)((Servo_GetCtrlMode() & 0x03U) << 5);
  data[7] |= (uint8_t)((Servo_GetInnerLoop() & 1U) << 7);
}

static void Comm_SendStatus(uint8_t seq)
{
  uint8_t data[8];
  Comm_BuildStatus(data);
  Comm_CacheSend(CFG_CAN_STATUS_BASE + Comm_Nid(), COMM_CMD_GET_STATUS, seq, data);
}

/**
 * @brief 故障事件：不经过重发缓存，直接发一帧与 GET_STATUS 应答相同的状态帧。
 */
static void Comm_SendFaultEvent(void)
{
  uint8_t data[8];
  Comm_BuildStatus(data);
  Comm_SendRaw(CFG_CAN_STATUS_BASE + Comm_Nid(), data);
}

/**
 * @brief 关桥并清掉保持的实时目标，状态仍为 RUNNING，收到新实时命令后恢复输出。
 */
static void Comm_Disable(void)
{
#if CFG_MULTIAXIS
  MultiAxis_Stop();
#endif
#if (CFG_COMMISSION != 0U)
  Commission_Abort();
#endif
  const ServoMode_t previous = Servo_PauseForFlash();
  Servo_ResumeAfterFlash(previous);
  s_mode_cleared_rt = 1U;
}

/**
 * @brief 应答 GET_CURRENT_STATE。电流帧只在被请求时发送，不随运动反馈上报。
 */
static void Comm_SendCurrent(uint8_t seq)
{
  ServoTelemetry_t tel;
  uint8_t data[8];
  Servo_GetTelemetry(&tel);
  Comm_WriteI16Le(&data[0], Comm_FloatToI16(tel.id, 0.001f));
  Comm_WriteI16Le(&data[2], Comm_FloatToI16(tel.iq, 0.001f));
  Comm_WriteI16Le(&data[4], Comm_FloatToI16(tel.iq_ref, 0.001f));
  Comm_WriteI16Le(&data[6], Comm_FloatToI16(tel.vq, 0.0001f));
  Comm_CacheSend(CFG_CAN_CURRENT_BASE + Comm_Nid(), COMM_CMD_GET_CURRENT_STATE, seq, data);
}

static void Comm_SendCurrentConfig(uint8_t page, uint8_t seq)
{
  uint8_t data[8] = {page, seq, 0U, 0U, 0U, 0U, 0U, 0U};
  if (page == 0U)
  {
    Comm_WriteU16Le(&data[2], Comm_FloatToU16(CFG_I_LIMIT_A, 0.001f));
    Comm_WriteU16Le(&data[4], Comm_FloatToU16(CFG_V_LIMIT, 0.0001f));
#if (CFG_COMMISSION != 0U)
    Comm_WriteU16Le(&data[6], Comm_FloatToU16(Commission_CurrentBandwidth(), 1.0f));
#else
    Comm_WriteU16Le(&data[6], Comm_FloatToU16(CFG_CUR_BW_HZ, 1.0f));
#endif
  }
  else if (page == 1U)
  {
    float kp, ki;
    Servo_GetCurrentGains(&kp, &ki);
    Comm_WriteU16Le(&data[2], Comm_FloatToU16(kp, 0.001f));
    Comm_WriteU16Le(&data[4], Comm_FloatToU16(ki, 1.0f));
    Comm_WriteU16Le(&data[6], (uint16_t)CFG_PWM_HZ);
  }
  else if (page == 2U)
  {
    Comm_WriteU16Le(&data[2], Comm_FloatToU16(CFG_MOTOR_KT_NM_A, 0.001f));
    Comm_WriteU16Le(&data[4], Comm_FloatToU16(CFG_GEAR_RATIO, 0.01f));
  }
  Comm_CacheSend(CFG_CAN_CURRENT_CONFIG_BASE + Comm_Nid(), COMM_CMD_GET_CURRENT_CONFIG, seq, data);
}

static void Comm_SendFeedback(void)
{
  ServoTelemetry_t tel;
  uint8_t data[8];

  Servo_GetTelemetry(&tel);
  Comm_WriteI32Le(&data[0], Comm_FloatToI32(tel.p_act, CFG_POS_LSB));
  Comm_WriteI16Le(&data[4], Comm_FloatToI16(tel.v_act, CFG_VEL_LSB));
  /* 运控指令对应的输出轴力矩，不是实测转矩。 */
  Comm_WriteI16Le(&data[6], Comm_FloatToI16(Cfg_PuToNm(tel.t_ref), CFG_TORQUE_LSB));
  Comm_SendRaw(CFG_CAN_FB_BASE + Comm_Nid(), data);
}

static uint8_t Comm_ParseMotion(const uint8_t *d, float *p, float *v, float *ff)
{
  const float vel = (float)((int16_t)Comm_ReadU16Le(&d[4])) * CFG_VEL_LSB;

  *p = (float)Comm_ReadI32Le(&d[0]) * CFG_POS_LSB;
  *v = Foc_Clamp(vel, CFG_VEL_CMD_MIN, CFG_VEL_CMD_MAX);
  *ff = Foc_Clamp(Cfg_NmToPu((float)((int16_t)Comm_ReadU16Le(&d[6])) * CFG_TORQUE_LSB),
                 -1.0f, 1.0f);
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
    kp = Cfg_NmToPu((float)kp_raw * CFG_KP_LSB);
    kd = Cfg_NmToPu((float)kd_raw * CFG_KD_LSB);
    if ((!(CFG_MOTOR_KT_NM_A > 0.0f) && ((kp_raw != 0U) || (kd_raw != 0U))) ||
        (kp < CFG_KP_MIN) || (kp > CFG_KP_MAX) || (kd < CFG_KD_MIN) || (kd > CFG_KD_MAX))
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
    Comm_SendGainsReply(seq, mode, Cfg_PuToNm(kp), 0.0f, Cfg_PuToNm(kd),
                        CFG_KP_LSB, CFG_KP_LSB, CFG_KD_LSB);
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
  ServoMode_t previous;

  Servo_GetVelocityGains(&vel_kp, &vel_ki);
  Servo_GetPositionGains(&pos_kp, &pos_ki, &pos_kd);
  previous = Servo_PauseForFlash();
#if (CFG_COMMISSION != 0U)
  if (Commission_OwnsGains())
  {
    ok = Commission_SaveGains();
  }
  else
#endif
  {
    Can_StopForFlash();
    ok = CaliNv_SaveUserGains(Node_GetId(), Servo_GetInnerLoop(),
                              Comm_FloatToU16(vel_kp, CFG_KP_VEL_LSB),
                              Comm_FloatToU16(vel_ki, CFG_KI_VEL_LSB),
                              Comm_FloatToU16(pos_kp, CFG_KP_POS_LSB),
                              Comm_FloatToU16(pos_ki, CFG_KI_POS_LSB),
                              Comm_FloatToU16(pos_kd, CFG_KD_POS_LSB));
    Can_Restart();
    Adc_RefreshVbus();
  }
  Servo_ResumeAfterFlash(previous);
  Comm_FlushRx();
  s_drop_pending_rt = 1U;
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
    case COMM_CMD_DISABLE:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_Disable();
      Comm_SendAck(cmd, seq, COMM_RES_OK);
      break;

    case COMM_CMD_GET_FEEDBACK:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_SendFeedback();
      break;

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
      if ((Servo_GetWarning() & SERVO_WARN_ABS_UNKNOWN) != 0U)
      {
        /** 多圈位置没建立时存下的零点，重启后会落到别的圈上。 */
        Comm_SendAck(cmd, seq, COMM_RES_INVALID_STATE);
        break;
      }
      {
        uint8_t ok;
        const ServoMode_t previous = Servo_PauseForFlash();
        Servo_SetZero();
        Can_StopForFlash();
#if (CFG_COMMISSION != 0U)
        ok = CaliNv_SaveMtZero(Commission_LegacyZero(Servo_GetZeroMotor()));
#else
        ok = CaliNv_SaveMtZero(Servo_GetZeroMotor());
#endif
        Can_Restart();
        Adc_RefreshVbus();
        Servo_ResumeAfterFlash(previous);
        Comm_FlushRx();
        s_drop_pending_rt = 1U;
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
        if ((Servo_GetFault() != 0U) && (Servo_ClearFault() == 0U))
        {
          Comm_SendAck(cmd, seq, COMM_RES_FAULT_ACTIVE);
          break;
        }
        s_fault = 0U;
        s_state = COMM_STATE_RUNNING;
        Servo_SetMode(SERVO_IDLE);
        Pwm_DisableOutputs();
        if (s_calibrated != 0U) { Servo_ResumeAfterFlash(SERVO_RUN); }
        Comm_FlushRx();
        s_drop_pending_rt = 1U;
      }
      Comm_SendAck(cmd, seq, COMM_RES_OK);
      break;

    case COMM_CMD_SET_INNER_LOOP:
      if ((d[2] > CFG_INNER_LOOP_CURRENT) || (Comm_ReservedZero(d, 3U) == 0U))
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      if ((s_state != COMM_STATE_RUNNING) ||
          ((d[2] == CFG_INNER_LOOP_CURRENT) &&
           ((BOARD_CURRENT_CONTROL_SUPPORTED == 0U) || (Adc_CurrentSenseReady() == 0U))))
      {
        Comm_SendAck(cmd, seq, COMM_RES_INVALID_STATE);
        break;
      }
      {
        const ServoMode_t previous = Servo_PauseForFlash();
        Servo_SetInnerLoop(d[2]);
#if (CFG_COMMISSION != 0U)
        Commission_ApplyControl();
#endif
        Servo_ResumeAfterFlash(previous);
      }
      s_mode_cleared_rt = 1U;
      Comm_SendAckArg(cmd, seq, COMM_RES_OK, d[2]);
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
      Servo_HoldPosition();
      if (Node_ApplyAndReset(d[2]) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_INTERNAL_ERROR);
        break;
      }
      Comm_SendAckArg(cmd, seq, COMM_RES_OK, d[2]);
      break;

    case COMM_CMD_GET_DEVICE_INFO:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_SendDeviceInfo(seq);
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

    case COMM_CMD_GET_CURRENT_CONFIG:
      if ((d[2] > 2U) || (Comm_ReservedZero(d, 3U) == 0U))
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_SendCurrentConfig(d[2], seq);
      break;

    case COMM_CMD_GET_CURRENT_STATE:
      if (Comm_ReservedZero(d, 2U) == 0U)
      {
        Comm_SendAck(cmd, seq, COMM_RES_OUT_OF_RANGE);
        break;
      }
      Comm_SendCurrent(seq);
      break;

    default:
      Comm_SendAck(cmd, seq, COMM_RES_INVALID_COMMAND);
      break;
  }
}

void Comm_Init(uint8_t cali_ok)
{
  s_calibrated = cali_ok;
  s_ctrl_mode = COMM_MODE_MOTION;
  s_have_cache = 0U;
  s_drop_pending_rt = 0U;
  s_mode_cleared_rt = 0U;
  s_fault_event = 0U;
  s_warning_seen = 0U;
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

  Comm_SyncFault();
  while (Can_PopRx(&frame) != 0U)
  {
    Comm_SyncFault();
#if CFG_MULTIAXIS
    if (frame.id == CFG_CAN_MGMT_BASE + nid && frame.data[0] >= 0x40U && frame.data[0] <= 0x44U)
    {
#if CFG_COMMISSION
      if (Commission_BlocksDrive()) { have_rt = 0U; continue; }
#endif
      MultiAxis_Command(frame.data, (uint8_t)nid, s_calibrated);
      have_rt = 0U;
      continue;
    }
    if (MultiAxis_Busy() &&
        !((frame.id == CFG_CAN_MGMT_BASE + nid || frame.id == CFG_CAN_BROADCAST_ID) &&
          frame.data[0] == COMM_CMD_DISABLE))
    { have_rt = 0U; continue; }
#endif
#if (CFG_COMMISSION != 0U)
    if (frame.id == CFG_CAN_MGMT_BASE + nid &&
        frame.data[0] >= 0x30U && frame.data[0] <= 0x37U)
    {
      if (Commission_Handle(frame.data, nid, s_calibrated))
      {
        s_calibrated = 1U;
        s_state = COMM_STATE_RUNNING;
        s_fault = 0U;
      }
      have_rt = 0U;
      continue;
    }
    if (Commission_BlocksDrive() &&
        !((frame.id == CFG_CAN_MGMT_BASE + nid || frame.id == CFG_CAN_BROADCAST_ID) &&
          frame.data[0] == COMM_CMD_DISABLE))
    {
      have_rt = 0U;
      continue;
    }
#endif
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
    else if ((frame.id == CFG_CAN_BROADCAST_ID) && (frame.data[0] == COMM_CMD_DISABLE) &&
             (Comm_ReservedZero(frame.data, 2U) != 0U))
    {
      /** @brief 广播停止不回 ACK，否则全部节点会同时应答。 */
      Comm_Disable();
      s_mode_cleared_rt = 0U;
      have_rt = 0U;
    }
  }

  Comm_SyncFault();
  if (s_fault_event != 0U)
  {
    s_fault_event = 0U;
    Comm_SendFaultEvent();
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

uint8_t Comm_IsCalibrated(void) { return s_calibrated; }
