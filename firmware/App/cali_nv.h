#ifndef CALI_NV_H
#define CALI_NV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t pole_pairs;
  int8_t  encoder_dir;
  int8_t  closed_loop_dir;
  uint8_t node_id;
  uint8_t user_gains_valid;
  uint8_t user_gains_inner_loop;
  uint16_t user_gains_scale_milli;
  float   electrical_offset_rad;
  uint16_t vel_kp_raw;
  uint16_t vel_ki_raw;
  uint16_t pos_kp_raw;
  uint16_t pos_ki_raw;
  uint16_t pos_kd_raw;
  float   mt_zero_rad;
  /** @brief 游标第二编码器零偏是否已校准，以及零偏值（1/256 圈号）。 */
  uint8_t enc2_offset_valid;
  int8_t  enc2_offset_raw;
} CaliNvData_t;

uint8_t CaliNv_Load(CaliNvData_t *out);
uint8_t CaliNv_LoadMetadata(CaliNvData_t *out);
uint8_t CaliNv_Save(const CaliNvData_t *in);
uint8_t CaliNv_LoadNodeId(void);
uint8_t CaliNv_SaveNodeId(uint8_t node_id);
uint8_t CaliNv_SaveUserGains(uint8_t node_id,
                             uint8_t inner_loop,
                             uint16_t vel_kp_raw,
                             uint16_t vel_ki_raw,
                             uint16_t pos_kp_raw,
                             uint16_t pos_ki_raw,
                             uint16_t pos_kd_raw);
uint8_t CaliNv_SaveMtZero(float mt_zero_rad);

#ifdef __cplusplus
}
#endif

#endif /* CALI_NV_H */
