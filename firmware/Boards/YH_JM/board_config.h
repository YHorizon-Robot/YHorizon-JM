#ifndef YH_JM_BOARD_CONFIG_H
#define YH_JM_BOARD_CONFIG_H

/**
 * @brief YH_JM：GD32F303，32 MHz 晶振，双编码器游标，WK4310 + 减速比 8。
 */
#define BOARD_NAME                  "YH_JM"
#define BOARD_ID                    (1U)
#define BOARD_HAS_ENCODER2          (1U)
#define BOARD_HAS_CURRENT_SENSE     (1U)
#define BOARD_CURRENT_CONTROL_SUPPORTED (1U)
/* INA2181: positive motor phase current flows ground -> low-side shunt. */
#define CFG_CURRENT_SENSE_SIGN      (-1.0f)
#define BOARD_DEADTIME_NS           (100U)
#define BOARD_NV_PAGE_ADDR          (0x0803F800UL)

#define CFG_HXTAL_HZ                (32000000UL)
#define CFG_SYSCLK_HZ               (112000000UL)
#define BOARD_APB1_HZ               (CFG_SYSCLK_HZ / 2UL)
#define BOARD_APB2_HZ               (CFG_SYSCLK_HZ)

#define CFG_ENCODER_TYPE            CFG_ENCODER_MT6701

#define CFG_POLE_PAIRS              (14U)
#define CFG_MOTOR_R_LL_OHM          (10.32f)
#define CFG_MOTOR_L_LL_H            (4.76e-3f)
#ifndef CFG_CURRENT_EARLY_PWM
#define CFG_CURRENT_EARLY_PWM       (1U)
#endif
/* WK4310: 425 Hz with early PWM loading, 350 Hz on the valley-only path. */
#ifndef CFG_CUR_BW_HZ
#if CFG_CURRENT_EARLY_PWM
#define CFG_CUR_BW_HZ               (425.0f)
#else
#define CFG_CUR_BW_HZ               (350.0f)
#endif
#endif
#ifndef CFG_I_LPF_HZ
#define CFG_I_LPF_HZ                (1500.0f)
#endif
/** @brief 转矩常数，Nm/A。对应电流环的相电流峰值 Iq，不是有效值。 */
#define CFG_MOTOR_KT_NM_A           (0.23f)
#define CFG_VBUS_NOMINAL_V          (24.0f)
#define CFG_VBUS_SCALE              (31.303f)

#define CFG_VERNIER_TEETH1          (30U)
#define CFG_VERNIER_TEETH2          (31U)
#define CFG_GEAR_RATIO              (8.0f)

#endif /* YH_JM_BOARD_CONFIG_H */
