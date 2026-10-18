#ifndef YHJM_CONFIG_H
#define YHJM_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_NODE_ID                 (1U)
#define CFG_NODE_ID_MIN             (1U)
#define CFG_NODE_ID_MAX             (63U)
#define CFG_POLE_PAIRS              (14U)

/**
 * @name Motor electrical (WK4310)
 * @brief Datasheet line-to-line R/L, halved to star phase for the current PI.
 *        Set @ref CFG_VBUS_NOMINAL_V to 24 or 48; PI also tracks measured Vbus.
 */
/** @{ */
#define CFG_MOTOR_R_LL_OHM          (10.32f)
#define CFG_MOTOR_L_LL_H            (4.76e-3f)
#define CFG_MOTOR_R_OHM             (CFG_MOTOR_R_LL_OHM * 0.5f)
#define CFG_MOTOR_L_H               (CFG_MOTOR_L_LL_H * 0.5f)
#define CFG_VBUS_NOMINAL_V          (24.0f)   /* 48.0f if 48 V pack */
#define CFG_VBUS_GAIN_MIN_V         (8.0f)
/** @} */

#define CFG_ENCODER_MT6701          (0U)
#define CFG_ENCODER_KTH7812         (1U)
#define CFG_ENCODER_TYPE            CFG_ENCODER_MT6701

/**
 * @name Vernier dual encoder
 * @brief Motor-end 30T + aux 31T recover 31 unique motor turns; output = motor / 8.
 */
/** @{ */
#define CFG_VERNIER_TEETH1          (30U)
#define CFG_VERNIER_TEETH2          (31U)
#define CFG_ENC2_DIR                (1)
#define CFG_ENC2_OFFSET_RAD         (0.0f)
#define CFG_GEAR_RATIO              (8.0f)
#define CFG_GEAR_RATIO_INV          (0.125f)
#define CFG_MULTI_TURN_ZERO_RAD     (0.0f)
/** @} */

#define CFG_HXTAL_HZ                (32000000UL)
#define CFG_SYSCLK_HZ               (112000000UL) /* HXTAL 32 MHz / 2 * 7 */
#define CFG_PWM_HZ                  (20000UL)
#define CFG_CTRL_HZ                 (4000UL)
#define CFG_PWM_DT_S                (1.0f / (float)CFG_PWM_HZ)
#define CFG_CTRL_DT_S               (1.0f / (float)CFG_CTRL_HZ)

#define CFG_TIM1_ARR                ((uint32_t)((CFG_SYSCLK_HZ / (2UL * CFG_PWM_HZ)) - 1UL))
#define CFG_TIM1_DEADTIME_DTG       (11U)   /* ~98 ns at 112 MHz, CKD=1 */
#define CFG_CTRL_TIM_PSC            ((CFG_SYSCLK_HZ / (CFG_CTRL_HZ * 250UL)) - 1UL)
#define CFG_CTRL_TIM_ARR            (249U)

#define CFG_V_LIMIT                 (0.50f)    /* pu; also clamps cali Vd/Vq */
#define CFG_V_SLEW_PU_S             (100.0f)   /* max |d(Vd,Vq)/dt|, pu/s */
#define CFG_VEL_LPF_HZ              (500.0f)  /* 2nd-order Butterworth on d(theta_e)/dt */

/**
 * @name Inner loop
 * @brief Outer motion/vel/pos always compute t_ref. Voltage: t_ref is Vq.
 *        Current: t_ref is mapped to Iq_ref, Id/Iq PI outputs Vd/Vq.
 *        Sampling always runs. Default is the current loop. If the motor R/L
 *        differ from WK4310, set @ref CFG_INNER_LOOP to @ref CFG_INNER_LOOP_VOLTAGE.
 */
/** @{ */
#define CFG_INNER_LOOP_VOLTAGE      (0U)
#define CFG_INNER_LOOP_CURRENT      (1U)
#define CFG_INNER_LOOP              CFG_INNER_LOOP_CURRENT
#define CFG_SHUNT_OHM               (0.001f)
#define CFG_CSA_GAIN                (20.0f)
#define CFG_I_LIMIT_A               (5.0f)
#define CFG_CUR_BW_HZ               (500.0f)
/**
 * @brief Minimum ticks after the latest low-side turn-on before the current trigger.
 *        112 ticks is 1 µs at 112 MHz, including the ~98 ns dead time.
 */
#define CFG_ADC_I_BLANK_TICKS       (112U)
/**
 * @brief Ticks from the CH3 match until the CURB sample-and-hold ends.
 *        Injected channels use 1.5-cycle sampling: 1.5 + 12.5 + 1.5 ADC cycles.
 *        ADCCLK is APB2/8, so each ADC cycle is 8 timer ticks.
 */
#define CFG_ADC_I_SAMPLE_END_TICKS  (124U)
#define CFG_ADC_I_OFFSET_SAMPLES    (32U)
/** @} */

#define CFG_ADC_VREF_V              (3.3f)
#define CFG_ADC_FULLSCALE           (4095.0f)
#define CFG_VBUS_SCALE              (31.303f) /* R1=100k / R3=3.3k: Vbus = Vadc * (100+3.3)/3.3 */
#define CFG_VBUS_LSB                (0.01f)
#define CFG_VBUS_POLL_MS            (5U)

#define CFG_CAN_MOTION_BASE         (0x100U)
#define CFG_CAN_VEL_BASE            (0x140U)
#define CFG_CAN_GAINS_BASE          (0x180U)
#define CFG_CAN_POS_BASE            (0x1C0U)
#define CFG_CAN_MGMT_BASE           (0x200U)
#define CFG_CAN_ACK_BASE            (0x280U)
#define CFG_CAN_FB_BASE             (0x300U)
#define CFG_CAN_STATUS_BASE         (0x380U)
#define CFG_CAN_CALI_RPT_BASE       (0x3C0U)
#define CFG_CAN_RX_SLOTS            (16U)

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

#define CFG_VEL_CMD_MIN             (-3276.8f) /* int16 * 0.1 rad/s */
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

#define CFG_CALI_LOCK_V             (0.1f)
#define CFG_CALI_LOCK_MS            (300U)
#define CFG_CALI_ROTATE_ELEC_RAD_S  (3.141592653589793f) /* 0.5 elec rev/s */
#define CFG_CALI_PP_ELEC_REVS       (4U)   /* per direction; both ways still 8 elec revs */
#define CFG_CALI_ROTATE_MS          (2000U * CFG_CALI_PP_ELEC_REVS) /* 2π/rate = 2 s/rev */
#define CFG_CALI_ROTATE_SAMPLE_MS   (10U)
#define CFG_CALI_OFFSET_SKIP_MS     (250U)
#define CFG_CALI_SETTLE_MS          (40U)
#define CFG_CALI_MIN_MECH_DELTA     (0.1f)
#define CFG_CALI_PP_MIN             (2U)
#define CFG_CALI_PP_MAX             (30U)
#define CFG_CALI_PP_MAX_RESIDUAL    (0.40f)
#define CFG_CALI_PROBE_VQ           (0.05f)
#define CFG_CALI_PROBE_MS           (400U)
#define CFG_CALI_MIN_VEL            (0.3f)
#define CFG_CALI_REPORT_MS          (50U)

#define CFG_DEBUG_PERIOD_MS         (200U)

#define CFG_NVIC_TIM1               (1U)
#define CFG_NVIC_TIM6               (3U)
#define CFG_NVIC_CAN                (5U)

#ifdef __cplusplus
}
#endif

#endif /* YHJM_CONFIG_H */
