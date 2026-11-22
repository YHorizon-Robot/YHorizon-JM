#include "dm4310.h"

#include <math.h>
#include <string.h>

#define DM_P_MAX 12.5f
#define DM_V_MAX 30.0f
#define DM_T_MAX 10.0f

DmCommand Dm_Decode(const uint8_t d[8], DmMotion *m)
{
  static const uint8_t prefix[7] = {255, 255, 255, 255, 255, 255, 255};
  if (memcmp(d, prefix, sizeof(prefix)) == 0)
  {
    return (d[7] >= DM_CLEAR && d[7] <= DM_ZERO) ? (DmCommand)d[7] : DM_RESERVED;
  }
  Mit_Unpack(d, DM_V_MAX, DM_T_MAX, m);
  return DM_MOTION;
}

void Dm_Feedback(uint8_t d[8], uint8_t node, uint8_t status,
                 float position, float velocity, float torque, float board_temp)
{
  d[0] = (uint8_t)((status << 4) | (node & 15U));
  Mit_PackMeasurements(d, DM_V_MAX, DM_T_MAX, position, velocity, torque);
  d[6] = (uint8_t)fminf(255.0f, fmaxf(0.0f, board_temp));
  d[7] = 0U; /* No rotor temperature sensor on JM1. */
}

uint8_t Dm_ReadRegister(uint8_t node, const uint8_t d[8], uint8_t reply[8])
{
  uint32_t bits;
  if (d[0] != node || d[1] != 0U || d[2] != 0x33U || d[4] || d[5] || d[6] || d[7])
  { return 0U; }
  switch (d[3])
  {
    case 7: bits = Dm_FeedbackId(node); break;
    case 8: bits = node; break;
    case 9: bits = DM4310_TIMEOUT_MS * 20U; break; /* 50 us/LSB */
    case 10: bits = 1U; break; /* MIT */
    case 21:
    case 22:
    case 23:
    {
      static const float ranges[] = {DM_P_MAX, DM_V_MAX, DM_T_MAX};
      _Static_assert(sizeof(float) == sizeof(bits), "DM register requires binary32");
      memcpy(&bits, &ranges[d[3] - 21U], sizeof(bits));
      break;
    }
    default: return 0U;
  }
  memcpy(reply, d, 4U);
  for (unsigned i = 0; i < 4U; ++i) { reply[4U+i] = (uint8_t)(bits >> (8U*i)); }
  return 1U;
}
