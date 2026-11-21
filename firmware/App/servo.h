#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>
#include "foc_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  SERVO_IDLE = 0,
  SERVO_OPENLOOP,
  SERVO_VOLTAGE,
  SERVO_RUN,
  SERVO_FAULT
} ServoMode_t;

#define SERVO_CTRL_MOTION           (0x00U)
#define SERVO_CTRL_VELOCITY         (0x01U)
#define SERVO_CTRL_POSITION         (0x02U)

#define SERVO_FAULT_OVER_TEMP       (1U << 0)
#define SERVO_FAULT_OVERCURRENT     (1U << 1)
#define SERVO_FAULT_UNDERVOLTAGE    (1U << 3)
#define SERVO_FAULT_ENCODER        (1U << 4)
#define SERVO_FAULT_CURRENT_SENSE  (1U << 11)

/** @brief 板温达到 CFG_TEMP_WARN_C。与原协议 MOTOR_TEMPERATURE_WARNING 同位。 */
#define SERVO_WARN_TEMP            (1U << 0)
/**
 * @brief 双编码器板上电定圈失败，位置只在零点所在的那一圈内有效。
 *        占用原协议 POSITION_LIMIT_WARNING 的位置，含义由能力 bit5 声明。
 */
#define SERVO_WARN_ABS_UNKNOWN     (1U << 4)

typedef struct
{
  float p_set;
  float v_set;
  float kp;
  float kd;
  float t_ff;
  float t_ref;
  float p_act;
  float v_act;
  float elec;
  float ia;
  float ib;
  float id;
  float iq;
  float iq_ref;
  /** @brief 电机轴力矩，Nm。由实测 Iq 和 CFG_MOTOR_KT_NM_A 计算。 */
  float torque_nm;
  float vq;
  int8_t closed_loop_dir;
  uint8_t ctrl_mode;
  uint8_t inner_loop;
} ServoTelemetry_t;

void Servo_Init(void);
void Servo_OnPwmIsr(void);
/** Process a fresh conversion in steady closed-current mode. Returns 1 if consumed. */
uint8_t Servo_OnCurrentSampleIsr(void);
/** Valley after an early current iteration. Advance only the encoder and its protection. */
void Servo_OnPwmEncoderIsr(void);
void Servo_OnCtrlIsr(void);
/**
 * @brief  主循环里调用：板温监测；双编码器板定圈失败后的补定圈。
 */
void Servo_Service(void);
/**
 * @brief  最近一次板温。
 * @param[out] celsius_out  温度，°C。
 * @return 1 表示 NTC 在位且读数有效。
 */
uint8_t Servo_GetTemperature(float *celsius_out);

void Servo_SetMode(ServoMode_t mode);
ServoMode_t Servo_GetMode(void);
void Servo_SetOpenloop(float d_v, float q_v, float elec_rate_rad_s, float elec_angle_rad);
void Servo_SetOpenloopRate(float elec_rate_rad_s);
float Servo_GetOpenloopElec(void);
void Servo_SetVoltageCmd(float d_v, float q_v);
void Servo_SetMotion(float p_set, float v_set, float t_ff);
void Servo_SetVelocityCmd(float v_set);
void Servo_SetPositionCmd(float p_set, float v_max);
void Servo_SetGains(float kp, float kd);
void Servo_GetGains(float *kp, float *kd);
void Servo_SetVelocityGains(float kp, float ki);
void Servo_GetVelocityGains(float *kp, float *ki);
void Servo_SetPositionGains(float kp, float ki, float kd);
void Servo_GetPositionGains(float *kp, float *ki, float *kd);
void Servo_SetCurrentGains(float kp, float ki);
void Servo_GetCurrentGains(float *kp, float *ki);
void Servo_SetCtrlMode(uint8_t mode);
uint8_t Servo_GetCtrlMode(void);

/**
 * @brief  选择运控内环：`CFG_INNER_LOOP_VOLTAGE` 或 `CFG_INNER_LOOP_CURRENT`。
 *         开环/电压校准路径始终走电压；切换时复位电流 PI。
 */
void Servo_SetInnerLoop(uint8_t mode);
uint8_t Servo_GetInnerLoop(void);
uint16_t Servo_GetFault(void);
uint8_t Servo_ClearFault(void);
/**
 * @brief 中性占空比开桥，并丢弃随后 CFG_PWM_ENABLE_BLANK 个电流采样。
 * @return 有故障、采样未就绪或母线欠压时返回 0，不开桥。
 */
uint8_t Servo_EnableCalibrationOutputs(void);
/* Gate the bridge before blocking Flash access. Resume requires a new command. */
ServoMode_t Servo_PauseForFlash(void);
void Servo_ResumeAfterFlash(ServoMode_t previous);
void Servo_SetZero(void);
/**
 * @brief  编码器已连续 CFG_ENC_STABLE_SAMPLES 个 PWM 周期读到有效帧。
 */
uint8_t Servo_EncoderStable(void);
/**
 * @brief  建立多圈位置。
 *
 * 双编码器板要求连续多次游标解码一致，并在等价角度里取离零点最近的一个；
 * 单编码器板取零点所在的那一圈。
 *
 * @param[in] zero_motor_rad  保存的零点，电机端角度。
 * @return 1 表示成功；失败时调用方应改用 Servo_SeedFallback。
 */
uint8_t Servo_SeedMultiTurn(float zero_motor_rad);
/**
 * @brief  定圈失败时的退路：按单圈角度取零点所在的那一圈。
 *
 * 双编码器板同时置 SERVO_WARN_ABS_UNKNOWN，之后在关桥且无指令时由 Servo_Service 补定圈。
 *
 * @param[in] raw_mech_rad    电机编码器单圈角度（传感器方向）。
 * @param[in] zero_motor_rad  保存的零点，电机端角度。
 */
void Servo_SeedFallback(float raw_mech_rad, float zero_motor_rad);
/**
 * @brief  同时取一组电机编码器和第二编码器的计数，供校准测量游标零偏。
 * @param[out] raw1_out  电机编码器最近一次有效计数。
 * @param[out] raw2_out  第二编码器计数。
 * @return 1 表示两路都有效；没有第二编码器的板始终返回 0。
 */
uint8_t Servo_SampleVernier(uint16_t *raw1_out, uint16_t *raw2_out);
/**
 * @brief  当前警告位，SERVO_WARN_*。
 */
uint8_t Servo_GetWarning(void);
float Servo_GetZeroMotor(void);
void Servo_SetClosedLoopDir(int8_t dir);
void Servo_SetEncoderAlignment(int8_t encoder_dir, float electrical_offset_rad);
void Servo_SetPolePairs(uint8_t pole_pairs);
void Servo_HoldPosition(void);
void Servo_GetTelemetry(ServoTelemetry_t *out);
Foc_Encoder_t *Servo_GetEncoder(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_H */
