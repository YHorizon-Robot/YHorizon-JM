#ifndef DM4310_H
#define DM4310_H

#include "mit_wire.h"

#define DM4310_TIMEOUT_MS 100U
#define DM4310_REGISTER_ID 0x7FFU
#define DM4310_MAX_NODE 15U

typedef enum
{
  DM_MOTION,
  DM_CLEAR = 0xFB,
  DM_ENABLE = 0xFC,
  DM_DISABLE = 0xFD,
  DM_ZERO = 0xFE,
  DM_RESERVED = 0xFF
} DmCommand;

typedef MitMotion DmMotion;

static inline uint16_t Dm_FeedbackId(uint8_t node) { return 0x10U + node; }
DmCommand Dm_Decode(const uint8_t data[8], DmMotion *motion);
void Dm_Feedback(uint8_t data[8], uint8_t node, uint8_t status,
                 float position, float velocity, float torque, float board_temp);
uint8_t Dm_ReadRegister(uint8_t node, const uint8_t request[8], uint8_t reply[8]);

#endif
