#ifndef JM1_MIT_WIRE_H
#define JM1_MIT_WIRE_H

#include <stdint.h>
#include <math.h>

typedef struct {
  float position, velocity, kp, kd, torque;
} MitMotion;

static inline float DecodeSigned(uint16_t value, uint16_t max_code, float limit)
{
  return (float)value * (2.0f * limit / (float)max_code) - limit;
}

static inline uint16_t EncodeSigned(float value, uint16_t max_code, float limit)
{
  if (!isfinite(value)) { value = 0.0f; }
  if (value <= -limit) { return 0U; }
  if (value >= limit) { return max_code; }
  return (uint16_t)((value + limit) * ((float)max_code / (2.0f * limit)));
}

static inline void Mit_Unpack(const uint8_t d[8], float max_velocity, float max_torque, MitMotion *m)
{
  m->position = DecodeSigned((d[0] << 8) | d[1], 65535U, 12.5f);
  m->velocity = DecodeSigned((d[2] << 4) | (d[3] >> 4), 4095U, max_velocity);
  m->kp = (((d[3] & 15U) << 8) | d[4]) * (500.0f / 4095.0f);
  m->kd = ((d[5] << 4) | (d[6] >> 4)) * (5.0f / 4095.0f);
  m->torque = DecodeSigned(((d[6] & 15U) << 8) | d[7], 4095U, max_torque);
}

static inline void Mit_PackMeasurements(uint8_t d[8], float max_velocity, float max_torque,
                                        float position, float velocity, float torque)
{
  const uint16_t p = EncodeSigned(position, 65535U, 12.5f);
  const uint16_t v = EncodeSigned(velocity, 4095U, max_velocity);
  const uint16_t t = EncodeSigned(torque, 4095U, max_torque);
  d[1] = (uint8_t)(p >> 8);
  d[2] = (uint8_t)p;
  d[3] = (uint8_t)(v >> 4);
  d[4] = (uint8_t)((v << 4) | (t >> 8));
  d[5] = (uint8_t)t;
}

#endif
