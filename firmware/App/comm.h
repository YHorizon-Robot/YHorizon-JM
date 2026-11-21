#ifndef COMM_H
#define COMM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Comm_Init(uint8_t cali_ok);
void Comm_Process(void);
uint8_t Comm_IsCalibrated(void);

#ifdef __cplusplus
}
#endif

#endif /* COMM_H */
