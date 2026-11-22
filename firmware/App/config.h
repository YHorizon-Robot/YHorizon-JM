#ifndef YFOC_CONFIG_H
#define YFOC_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_ENCODER_MT6701          (0U)
#define CFG_ENCODER_KTH7812         (1U)

#include "board_config.h"

#define CFG_NODE_ID                 (1U)
#define CFG_NODE_ID_MIN             (1U)
#define CFG_NODE_ID_MAX             (63U)

#ifndef CFG_POLE_PAIRS
#define CFG_POLE_PAIRS              (14U)
#endif

#ifndef CFG_MOTOR_R_LL_OHM
#define CFG_MOTOR_R_LL_OHM          (10.32f)
#endif
#ifndef CFG_MOTOR_L_LL_H
#define CFG_MOTOR_L_LL_H            (4.76e-3f)
#endif
#define CFG_MOTOR_R_OHM             (CFG_MOTOR_R_LL_OHM * 0.5f)
#define CFG_MOTOR_L_H               (CFG_MOTOR_L_LL_H * 0.5f)
/**
 * @brief 转矩常数，Nm/A，对应相电流峰值。未写在板卡配置里时力矩按 0 计算。
 */
#ifndef CFG_MOTOR_KT_NM_A
#define CFG_MOTOR_KT_NM_A           (0.0f)
#endif
#ifndef CFG_VBUS_NOMINAL_V
#define CFG_VBUS_NOMINAL_V          (24.0f)
#endif
#define CFG_VBUS_GAIN_MIN_V         (8.0f)

#ifndef CFG_ENCODER_TYPE
#define CFG_ENCODER_TYPE            CFG_ENCODER_MT6701
#endif

/**
 * @name Vernier dual encoder
 * @brief Motor-end teeth1 + aux teeth2. Output position is motor angle / gear ratio.
 *        Boards without a second encoder ignore the tooth counts.
 */
/** @{ */
#ifndef CFG_VERNIER_TEETH1
#define CFG_VERNIER_TEETH1          (30U)
#endif
#ifndef CFG_VERNIER_TEETH2
#define CFG_VERNIER_TEETH2          (31U)
#endif
#define CFG_ENC2_DIR                (1)
#define CFG_ENC2_OFFSET_RAD         (0.0f)
/**
 * @brief 圈号余数离整数超过这个值就不采信本次游标解码。0.5 是相邻圈的分界。
 *
 * YH_JM 实测：零偏修正后余数在一圈内摆动，三圈范围内落在 -0.37..+0.33
 *（编码器非线性叠加齿隙），读数噪声约 0.02。取 0.40 让这样的电机全程可解，
 * 同时留出 0.40..0.60 的拒收带。
 */
#define CFG_VERNIER_RESIDUE_TOL     (0.40f)
#ifndef CFG_GEAR_RATIO
#define CFG_GEAR_RATIO              (8.0f)
#endif
#define CFG_GEAR_RATIO_INV          (1.0f / CFG_GEAR_RATIO)
#define CFG_MULTI_TURN_ZERO_RAD     (0.0f)
/** @} */

#ifndef CFG_HXTAL_HZ
#define CFG_HXTAL_HZ                (32000000UL)
#endif
#ifndef CFG_SYSCLK_HZ
#define CFG_SYSCLK_HZ               (112000000UL)
#endif
#define CFG_PWM_HZ                  (20000UL)
#define CFG_CTRL_HZ                 (4000UL)
#define CFG_PWM_DT_S                (1.0f / (float)CFG_PWM_HZ)
#define CFG_CTRL_DT_S               (1.0f / (float)CFG_CTRL_HZ)

#define CFG_TIM1_ARR                ((uint32_t)((CFG_SYSCLK_HZ / (2UL * CFG_PWM_HZ)) - 1UL))

/**
 * @brief 高级定时器 CKD=DIV1 时，DTG 一档等于一个内核时钟。
 *        100 ns 在 112 MHz 为 11，120 MHz 为 12，170 MHz 为 17。
 */
#ifndef BOARD_DEADTIME_NS
#define BOARD_DEADTIME_NS           (100U)
#endif
#define CFG_TIM1_DEADTIME_DTG       ((uint32_t)(((uint64_t)BOARD_DEADTIME_NS * (uint64_t)CFG_SYSCLK_HZ + 500000000ULL) / 1000000000ULL))
#define CFG_CTRL_TIM_PSC            ((CFG_SYSCLK_HZ / (CFG_CTRL_HZ * 250UL)) - 1UL)
#define CFG_CTRL_TIM_ARR            (249U)

#define CFG_V_LIMIT                 (0.45f)
#define CFG_V_SLEW_PU_S             (100.0f)
#define CFG_VEL_LPF_HZ              (500.0f)

#define CFG_INNER_LOOP_VOLTAGE      (0U)
#define CFG_INNER_LOOP_CURRENT      (1U)
#if (BOARD_CURRENT_CONTROL_SUPPORTED != 0U)
#define CFG_INNER_LOOP              CFG_INNER_LOOP_CURRENT
#else
#define CFG_INNER_LOOP              CFG_INNER_LOOP_VOLTAGE
#endif
#define CFG_SHUNT_OHM               (0.001f)
#define CFG_CSA_GAIN                (20.0f)
/**
 * @brief 三块板共用的指令电流上限和过流阈值。
 *        连续 CFG_I_TRIP_CONFIRM 个有效采样超过跳闸值才关桥。
 */
#define CFG_I_LIMIT_A               (20.0f)
#if CFG_CAN_COMPAT
/* Initial bench image: cap total q-axis demand, including cogging compensation. */
#define CFG_DRIVE_I_LIMIT_A          (1.0f)
#else
#define CFG_DRIVE_I_LIMIT_A          CFG_I_LIMIT_A
#endif

/**
 * @brief 电机轴力矩，单位 Nm。Iq 为相电流峰值。
 *        输出轴理想力矩再乘 CFG_GEAR_RATIO，不含效率。
 */
static inline float Cfg_MotorTorqueNm(float iq_a)
{
  return CFG_MOTOR_KT_NM_A * iq_a;
}

/**
 * @brief 当前母线下堵转、Id 为 0 时电机轴能达到的力矩。
 *        电流取软件上限和相电压/相电阻的较小值，相电压为 Vbus × CFG_V_LIMIT。
 */
static inline float Cfg_StallTorqueNm(float vbus_v, float phase_resistance)
{
  float i_avail;

  if (!(vbus_v > 0.0f) || !(phase_resistance > 0.0f))
  {
    return 0.0f;
  }
  i_avail = (CFG_V_LIMIT * vbus_v) / phase_resistance;
  if (i_avail > CFG_DRIVE_I_LIMIT_A)
  {
    i_avail = CFG_DRIVE_I_LIMIT_A;
  }
  return Cfg_MotorTorqueNm(i_avail);
}

/**
 * @brief 1 pu 指令对应的输出轴力矩，Nm。
 *        输出轴力矩 = 电机轴力矩 × 减速比。Kt 未配置时为 0。
 */
static inline float Cfg_OutputNmPerPu(void)
{
  if (!(CFG_V_LIMIT > 0.0f))
  {
    return 0.0f;
  }
  return (CFG_MOTOR_KT_NM_A * CFG_I_LIMIT_A * CFG_GEAR_RATIO) / CFG_V_LIMIT;
}

/**
 * @brief 输出轴牛·米换成电流环标幺指令。
 */
static inline float Cfg_NmToPu(float torque_nm)
{
  const float scale = Cfg_OutputNmPerPu();

  if (!(scale > 0.0f))
  {
    return 0.0f;
  }
  return torque_nm / scale;
}

/**
 * @brief 电流环标幺指令换成输出轴牛·米。
 */
static inline float Cfg_PuToNm(float pu)
{
  return pu * Cfg_OutputNmPerPu();
}
#ifndef CFG_CUR_BW_HZ
#define CFG_CUR_BW_HZ               (300.0f)
#endif
#ifndef CFG_CURRENT_EARLY_PWM
#define CFG_CURRENT_EARLY_PWM       (0U)
#endif
#define CFG_I_REF_SLEW_A_S          (1000.0f)
#define CFG_I_TRIP_A                (30.0f)
/** @brief 连续多少个有效采样超过 CFG_I_TRIP_A 才判过流。 */
#define CFG_I_TRIP_CONFIRM          (2U)
/**
 * @brief Id/Iq 一阶低通截止频率，单位 Hz。板卡可覆盖默认值。
 *        作用于电流环反馈和上报，过流比较仍使用未滤波的相电流。
 */
#ifndef CFG_I_LPF_HZ
#define CFG_I_LPF_HZ                (1000.0f)
#endif
/**
 * @brief 开桥后保持中性占空比并丢弃的采样数（自举电容充电、运放恢复）。
 *        这几拍里 ADC 贴轨同样丢弃。屏蔽结束后贴轨仍判电流采样故障。
 */
#define CFG_PWM_ENABLE_BLANK        (3U)
#define CFG_CURRENT_MISSING_MAX     (2U)
/* Low-side turn-on + amplifier settling before the first sample. */
#define CFG_ADC_I_BLANK_TICKS       (CFG_SYSCLK_HZ / 500000UL)
#define CFG_ADC_I_SAMPLE_END_TICKS  (124U)
#define CFG_ADC_I_OFFSET_SAMPLES    (32U)
#define CFG_ADC_I_OFFSET_MIN        (1500U)
#define CFG_ADC_I_OFFSET_MAX        (2600U)
#define CFG_ADC_I_RAW_MIN           (16U)
#define CFG_ADC_I_RAW_MAX           (4079U)

#define CFG_ADC_VREF_V              (3.3f)
#define CFG_ADC_FULLSCALE           (4095.0f)
#ifndef CFG_VBUS_SCALE
#define CFG_VBUS_SCALE              (31.303f)
#endif
#define CFG_VBUS_LSB                (0.01f)
/** @brief 母线采样周期。GD32 上 NTC 在 ADC1，母线固定在 ADC0 规则组。 */
#define CFG_VBUS_POLL_MS            (5U)
/** @brief 超过这么久没有新的成功采样，母线读数才作废。单次转换失败仍沿用上一笔。 */
#define CFG_VBUS_STALE_MS           (100U)
/** @brief 电流环里连续多少个 PWM 周期读到低于 8 V 才判欠压。20 kHz 下 200 次约 10 ms。 */
#define CFG_VBUS_UV_CONFIRM         (200U)

/**
 * @brief 板载 NTC（PA3）温度。没焊 NTC 的板子上电探测后不再采样，温度报 0，不报警告和故障。
 *
 * GD32 上 NTC 单独用 ADC1，不和母线、相电流共用转换器和标志。
 * G4 的 PA3 只接到 ADC1，温度仍和母线分时用 ADC1 规则组。
 * 温度连续 CFG_TEMP_CONFIRM 次（每次间隔 CFG_TEMP_POLL_MS）达到阈值才置警告或跳闸；
 * 警告降到 CFG_TEMP_WARN_CLEAR_C 以下才撤销，过温故障在温度低于该值后才能 CLEAR_FAULT。
 * 测的是驱动板温度，不是电机绕组温度。
 */
#define CFG_TEMP_POLL_MS            (100U)
#define CFG_TEMP_PROBE_SETTLE_MS    (20U)
#define CFG_TEMP_CONFIRM            (10U)
#define CFG_TEMP_WARN_C             (80.0f)
#define CFG_TEMP_WARN_CLEAR_C       (75.0f)
#define CFG_TEMP_FAULT_C            (100.0f)

#define CFG_CAN_MOTION_BASE         (0x100U)
#define CFG_CAN_VEL_BASE            (0x140U)
#define CFG_CAN_GAINS_BASE          (0x180U)
#define CFG_CAN_POS_BASE            (0x1C0U)
#define CFG_CAN_MGMT_BASE           (0x200U)
#define CFG_CAN_ACK_BASE            (0x280U)
#define CFG_CAN_FB_BASE             (0x300U)
#define CFG_CAN_STATUS_BASE         (0x380U)
#define CFG_CAN_CURRENT_BASE        (0x400U)
#define CFG_CAN_CURRENT_CONFIG_BASE (0x440U)
#define CFG_CAN_COMMISSION_BASE     (0x480U)
#define CFG_CAN_SYNC_ID             (0x080U)
#define CFG_CAN_SYNC_REPLY          (0x500U)
#define CFG_CAN_SYNC_FB             (0x540U)
#ifndef CFG_MULTIAXIS
#define CFG_MULTIAXIS               (0U)
#endif
#ifndef CFG_COMMISSION
#define CFG_COMMISSION              (0U)
#endif
/** @brief 广播管理帧 ID，即节点号 0 的管理帧，只接受 DISABLE。 */
#define CFG_CAN_BROADCAST_ID        (CFG_CAN_MGMT_BASE)
#define CFG_CAN_RX_SLOTS            (16U)

#ifndef BOARD_APB1_HZ
#define BOARD_APB1_HZ               (CFG_SYSCLK_HZ / 2UL)
#endif
#ifndef BOARD_APB2_HZ
#define BOARD_APB2_HZ               (CFG_SYSCLK_HZ)
#endif
#define CFG_CAN_BITRATE_HZ          (1000000UL)
#define CFG_CAN_PRESCALER           (4U)
#define CFG_CAN_BS2_TQ              (3U)
#define CFG_CAN_TQ_TOTAL            (BOARD_APB1_HZ / (CFG_CAN_PRESCALER * CFG_CAN_BITRATE_HZ))
#define CFG_CAN_BS1_TQ              (CFG_CAN_TQ_TOTAL - 1U - CFG_CAN_BS2_TQ)

#if (CFG_ENCODER_TYPE == CFG_ENCODER_KTH7812)
#define CFG_ENC_SCLK_MAX_HZ         (7500000UL)
#else
#define CFG_ENC_SCLK_MAX_HZ         (15000000UL)
#endif

#define CFG_POS_LSB                 (0.0001f)
#define CFG_VEL_LSB                 (0.1f)
#define CFG_KP_LSB                  (0.01f)
#define CFG_KD_LSB                  (0.001f)
#define CFG_KP_VEL_LSB              (0.001f)
#define CFG_KI_VEL_LSB              (0.001f)
#define CFG_KP_POS_LSB              (0.01f)
#define CFG_KI_POS_LSB              (0.001f)
#define CFG_KD_POS_LSB              (0.001f)
#define CFG_TORQUE_LSB              (0.001f)

#define CFG_VEL_CMD_MIN             (-3276.8f)
#define CFG_VEL_CMD_MAX             (3276.7f)
#define CFG_POS_VMAX_DEFAULT        (640.0f / CFG_GEAR_RATIO)
#define CFG_POS_ACC_DEFAULT         (80.0f / CFG_GEAR_RATIO)
#define CFG_POS_SETTLE_RAD          (1.0f / CFG_GEAR_RATIO)
#define CFG_VEL_KP_DEFAULT          (0.01f / CFG_GEAR_RATIO)
#define CFG_VEL_KI_DEFAULT          (0.3f / CFG_GEAR_RATIO)
#define CFG_POS_KP_DEFAULT          (3.0f / CFG_GEAR_RATIO)
#define CFG_POS_KI_DEFAULT          (0.1f / CFG_GEAR_RATIO)
#define CFG_POS_KD_DEFAULT          (0.0f)
#define CFG_KP_MIN                  (0.0f)
#define CFG_KP_MAX                  (5.0f)
#define CFG_KD_MIN                  (0.0f)
#define CFG_KD_MAX                  (1.0f)
#define CFG_KP_VEL_MAX              (1.0f)
#define CFG_KI_VEL_MAX              (10.0f)
#define CFG_KP_POS_MAX              (50.0f)
#define CFG_KI_POS_MAX              (5.0f)
#define CFG_KD_POS_MAX              (5.0f)

/** @brief 上电后编码器须连续有效的 PWM 采样数（10 ms），满足后才加载或开始校准。 */
#define CFG_ENC_STABLE_SAMPLES      (200U)
#define CFG_ENC_STARTUP_MS          (500U)

#define CFG_CALI_PP_MIN             (2U)
#define CFG_CALI_PP_MAX             (30U)

#define CFG_DEBUG_PERIOD_MS         (200U)

#ifndef CFG_FACTORY_TEST
#define CFG_FACTORY_TEST            (0U)
#endif
#define CFG_FACTORY_DRAG_V            (0.10f)
#define CFG_FACTORY_DRAG_ELEC_RAD_S   (3.14159265f)
#define CFG_FACTORY_PRINT_MS          (200U)
#define CFG_FACTORY_MOTION_MS         (1000U)
#define CFG_FACTORY_MOTION_MIN_CNT    (100U)
#define CFG_FACTORY_XTAL_SLOW_US      (20000UL)
#define CFG_FACTORY_VBUS_MIN_V        (8.0f)
#define CFG_FACTORY_VBUS_MAX_V        (60.0f)
#define CFG_FACTORY_IOFF_MIN          (1500U)
#define CFG_FACTORY_IOFF_MAX          (2600U)
#define CFG_FACTORY_I_OC_A            (3.0f)
#define CFG_FACTORY_I_MIN_RATIO       (0.35f)

#define CFG_FW_MAJOR                (1U)
#define CFG_FW_MINOR                (6U)

#define CFG_NVIC_PWM                (1U)
#define CFG_NVIC_CTRL               (3U)
#define CFG_NVIC_CAN                (5U)

#ifdef __cplusplus
}
#endif

#endif /* YFOC_CONFIG_H */
