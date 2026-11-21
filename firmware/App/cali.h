#ifndef CALI_H
#define CALI_H

#include <stdint.h>
#include "cali_nv.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t Cali_Start(void);
uint8_t Cali_ApplySaved(const CaliNvData_t *nv);

#ifdef __cplusplus
}
#endif

#endif /* CALI_H */
