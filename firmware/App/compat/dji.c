#include "profile.h"
#include "config.h"

#if CFG_C620
#define RAW_LIMIT 16384
#define AMPS_PER_COUNT (20.0f / 16384.0f)
#else
#define RAW_LIMIT 10000
#define AMPS_PER_COUNT (10.0f / 10000.0f)
#endif

ProfileAction ProfileDecode(uint8_t node, const CanFrame_t *f, ProfileTarget *t)
{
  const unsigned slot = (node - 1U) % 4U;
  const int16_t raw = (int16_t)ReadBE16(f->data + 2U * slot);
  if (raw > RAW_LIMIT || raw < -RAW_LIMIT) { return CMD_IGNORE; }
  t->amps = raw * AMPS_PER_COUNT;
  return raw ? CMD_CURRENT : CMD_NEUTRAL;
}

uint8_t ProfileReply(uint8_t node, const CanFrame_t *request, const ProfileState *s, CanFrame_t *out)
{
  if (request) { return 0; }
  out->id = 0x200U + node;
  out->dlc = 8;
  const float turns = s->rotor_angle / RAD_PER_TURN;
  const float phase = isfinite(turns) ? turns - floorf(turns) : 0.0f;
  WriteBE16(out->data, (uint16_t)(phase * 8192.0f) & 8191U);
  WriteBE16(out->data+2, ScaledI16(s->velocity * CFG_GEAR_RATIO * (60.0f / RAD_PER_TURN)));
  WriteBE16(out->data+4, ScaledI16(s->amps / AMPS_PER_COUNT));
#if CFG_C620
  out->data[6] = (uint8_t)fminf(255.0f, fmaxf(0.0f, s->temperature));
#endif
  return 1;
}
