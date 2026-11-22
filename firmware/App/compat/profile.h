#ifndef JM1_COMPAT_PROFILE_H
#define JM1_COMPAT_PROFILE_H

#include "can.h"
#include <math.h>
#include <string.h>

#define PROFILE_TIMEOUT_MS 100U
#define RAD_PER_DEG (0.017453292519943295f)
#define RAD_PER_TURN (6.283185307179586f)

#if CFG_RS05 || CFG_CYBERGEAR
#define PROFILE_EXT 1U
#define PROFILE_MASK 0xFFU
#elif CFG_HIGHTORQUE
#define PROFILE_EXT 1U
#define PROFILE_MASK 0x1FFF7FFFU /* source zero, reply-request bit variable */
#else
#define PROFILE_EXT 0U
#define PROFILE_MASK 0x7FFU
#endif

static inline uint8_t ProfileNodeValid(uint8_t node)
{
#if CFG_C610 || CFG_C620
  return node >= 1U && node <= 8U;
#elif CFG_RMD_X || CFG_LK_MF
  return node >= 1U && node <= 32U;
#else
  return node >= 1U && node <= 63U; /* JM1 native node range */
#endif
}

static inline uint32_t ProfileId(uint8_t node)
{
#if CFG_C620 || CFG_C610
  return node <= 4U ? 0x200U : 0x1FFU;
#elif CFG_RMD_X || CFG_LK_MF
  return 0x140U + node;
#else
  return node;
#endif
}

static inline uint8_t ProfileAccepts(uint8_t node, uint32_t id, uint8_t ext, uint8_t dlc)
{
#if CFG_HIGHTORQUE
  if (dlc != 2 && dlc != 3 && dlc != 4 && dlc != 8) { return 0; }
#else
  if (dlc != 8) { return 0; }
#endif
  return ProfileNodeValid(node) && ext == PROFILE_EXT &&
         id <= (ext ? 0x1FFFFFFFU : 0x7FFU) && (id & PROFILE_MASK) == ProfileId(node);
}

typedef enum {
  CMD_IGNORE, CMD_READ, CMD_ENABLE, CMD_STOP, CMD_CLEAR, CMD_NEUTRAL,
  CMD_RAM_ZERO, CMD_MOTION, CMD_CURRENT, CMD_VELOCITY, CMD_POSITION
} ProfileAction;

typedef struct {
  float position, velocity, kp, kd, torque, amps;
} ProfileTarget;

typedef struct {
  float position, velocity, torque, amps, rotor_angle, temperature, vbus;
  uint16_t fault;
  uint8_t enabled, calibrated;
  uint8_t uid[8];
} ProfileState;

ProfileAction ProfileDecode(uint8_t node, const CanFrame_t *request, ProfileTarget *target);
/* A null request denotes a periodic feedback tick for the DJI profiles. */
uint8_t ProfileReply(uint8_t node, const CanFrame_t *request, const ProfileState *state,
                     CanFrame_t *response);

static inline uint16_t ReadLE16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static inline uint16_t ReadBE16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline void WriteLE16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void WriteBE16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline int16_t ScaledI16(float x)
{
  return (int16_t)lroundf(fminf(32767.0f, fmaxf(-32768.0f, isfinite(x) ? x : 0.0f)));
}
static inline uint8_t BytesZero(const uint8_t *p, unsigned len)
{
  for (unsigned i = 0; i < len; ++i) { if (p[i]) { return 0; } }
  return 1;
}
#endif
