#include "profile.h"

#if CFG_LK_MF
#define AMPS_PER_COUNT (16.5f / 2048.0f)
#else
#define AMPS_PER_COUNT 0.01f
#endif

static int32_t ReadLE32(const uint8_t *p)
{
  uint32_t bits = ReadLE16(p) | ((uint32_t)ReadLE16(p+2) << 16);
  return (int32_t)bits;
}

ProfileAction ProfileDecode(uint8_t node, const CanFrame_t *f, ProfileTarget *t)
{
  (void)node;
  const uint8_t *d = f->data;
  switch (d[0])
  {
    case 0x9C: case 0x92: return BytesZero(d+1, 7) ? CMD_READ : CMD_IGNORE;
    case 0x80: case 0x81: return BytesZero(d+1, 7) ? CMD_STOP : CMD_IGNORE;
#if CFG_LK_MF
    case 0x88: return BytesZero(d+1, 7) ? CMD_ENABLE : CMD_IGNORE;
#endif
    case 0xA1:
    {
      if (!BytesZero(d+1, 3) || !BytesZero(d+6, 2)) { return CMD_IGNORE; }
      int16_t raw = (int16_t)ReadLE16(d+4);
#if CFG_LK_MF
      if (raw < -2048 || raw > 2048) { return CMD_IGNORE; }
#else
      if (!raw) { return CMD_NEUTRAL; }
#endif
      t->amps = raw * AMPS_PER_COUNT;
      return CMD_CURRENT;
    }
    case 0xA2:
      if (!BytesZero(d+1, 3)) { return CMD_IGNORE; }
      t->velocity = ReadLE32(d+4) * (0.01f * RAD_PER_DEG);
      return CMD_VELOCITY;
    case 0xA4:
      if (d[1] || ReadLE16(d+2) == 0U) { return CMD_IGNORE; }
      t->position = ReadLE32(d+4) * (0.01f * RAD_PER_DEG);
      t->velocity = ReadLE16(d+2) * RAD_PER_DEG;
      return CMD_POSITION;
    default: return CMD_IGNORE;
  }
}

uint8_t ProfileReply(uint8_t node, const CanFrame_t *f, const ProfileState *s, CanFrame_t *out)
{
  const uint8_t cmd = f->data[0];
  const float degrees = s->position / RAD_PER_DEG;
#if CFG_LK_MF
  out->id = 0x140U + node;
#else
  out->id = 0x240U + node;
#endif
  out->dlc = 8;
  out->data[0] = cmd;
  if (cmd == 0x92)
  {
#if CFG_LK_MF
    const unsigned start = 1;
    const double limit = 36028797018963968.0; /* signed 56-bit */
#else
    const unsigned start = 4;
    const double limit = 2147483648.0;
#endif
    double scaled = isfinite(degrees) ? (double)degrees * 100.0 : 0.0;
    uint64_t bits = (uint64_t)llround(fmin(limit - 1.0, fmax(-limit, scaled)));
#if CFG_LK_MF
    if (scaled >= limit) { bits = UINT64_C(0x7FFFFFFFFFFFFF); }
#endif
    for (unsigned i = start; i < 8; ++i) { out->data[i] = (uint8_t)bits; bits >>= 8; }
  }
  else if (cmd != 0x80 && cmd != 0x81 && cmd != 0x88)
  {
    out->data[1] = (uint8_t)(int8_t)fminf(127, fmaxf(-128, s->temperature));
#if CFG_LK_MF
    /* V2.35 gives MF-specific Iq for A1, and a 33 A scale for 9C/A2/A4. */
    const float feedback_lsb = cmd == 0xA1 ? AMPS_PER_COUNT : 33.0f / 2048.0f;
    WriteLE16(out->data+2, ScaledI16(fminf(2048, fmaxf(-2048, s->amps / feedback_lsb))));
#else
    WriteLE16(out->data+2, ScaledI16(s->amps / AMPS_PER_COUNT));
#endif
    WriteLE16(out->data+4, ScaledI16(s->velocity / RAD_PER_DEG));
#if CFG_LK_MF
    float turns = isfinite(degrees) ? degrees / 360.0f : 0.0f;
    WriteLE16(out->data+6, (uint16_t)((turns - floorf(turns)) * 16384.0f) & 0x3FFFU);
#else
    WriteLE16(out->data+6, ScaledI16(degrees));
#endif
  }
  return 1;
}
