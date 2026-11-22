#include "profile.h"

/* Classic-CAN STM32 example dialect (manual 3.2). */
ProfileAction ProfileDecode(uint8_t node, const CanFrame_t *f, ProfileTarget *t)
{
  (void)node;
  const uint8_t *d = f->data;
  if (d[0] == 0x17 && d[1] == 1 && (f->id & 0x8000) &&
      (f->dlc == 2 || (f->dlc == 8 && BytesZero(d+2, 6)))) { return CMD_READ; }
  if (f->dlc == 3 && d[0] == 1 && BytesZero(d+1, 2)) { return CMD_STOP; }
  if (f->dlc == 4 && d[0] == 5 && d[1] == 0x13)
  {
    int16_t raw = (int16_t)ReadLE16(d+2);
    if (raw == INT16_MIN) { return CMD_IGNORE; }
    t->torque = raw * 0.01f;
    return raw ? CMD_MOTION : CMD_NEUTRAL;
  }
  return CMD_IGNORE;
}

uint8_t ProfileReply(uint8_t node, const CanFrame_t *f, const ProfileState *s, CanFrame_t *out)
{
  if (f->data[0] != 0x17) { return 0; }
  out->id = (uint32_t)node << 8;
  out->extended = 1;
  out->dlc = 8;
  out->data[0] = 0x27; out->data[1] = 1;
  /* -32768 is reserved by the manufacturer's example. */
  WriteLE16(out->data+2, ScaledI16(fmaxf(-32767.0f, s->position * (10000.0f / RAD_PER_TURN))));
  WriteLE16(out->data+4, ScaledI16(fmaxf(-32767.0f, s->velocity * (4000.0f / RAD_PER_TURN))));
  WriteLE16(out->data+6, ScaledI16(fmaxf(-32767.0f, s->torque * 100.0f)));
  return 1;
}
