#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (CFG_ENCODER_TYPE == CFG_ENCODER_KTH7812)
#define ENCODER_CPR           65536U
#else
#define ENCODER_CPR           16384U
#endif
#define ENCODER_READ_INVALID  0xFFFFU

void Encoder_Init(void);

/**
 * @brief  Read the motor-end encoder (SPI0, CS=PA4).
 * @param[out] raw_out Angle code.
 * @return 1 on success.
 */
uint8_t Encoder_ReadRawFast(uint16_t *raw_out);
uint8_t Encoder_ReadFrame(uint16_t *raw_out, uint8_t *status_out);

/**
 * @brief  Read the vernier/aux encoder (SPI2, CS=PA15).
 * @param[out] raw_out Angle code.
 * @return 1 on success.
 */
uint8_t Encoder2_ReadRawFast(uint16_t *raw_out);
uint8_t Encoder2_ReadFrame(uint16_t *raw_out, uint8_t *status_out);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
