#include "profile.h"
#include "servo.h"

#define MASTER 0xFDU
#if CFG_RS05
#define COMMAND_P 12.57f
#define COMMAND_V 33.0f
#define COMMAND_T 14.0f
#define FEEDBACK_V 50.0f
#define FEEDBACK_T 5.5f
#else
#define COMMAND_P (2.0f * RAD_PER_TURN)
#define COMMAND_V 30.0f
#define COMMAND_T 12.0f
#define FEEDBACK_V COMMAND_V
#define FEEDBACK_T COMMAND_T
#endif

static float SignedRange(uint16_t raw, float limit) { return raw * (2.0f * limit / 65535.0f) - limit; }
static uint16_t PackRange(float value, float limit)
{
  float normalized = isfinite(value) ? value / limit : 0.0f;
  return (uint16_t)((fminf(1, fmaxf(-1, normalized)) + 1.0f) * 32767.5f);
}

ProfileAction ProfileDecode(uint8_t node, const CanFrame_t *f, ProfileTarget *t)
{
  (void)node;
  const uint32_t type = f->id >> 24;
  const uint16_t field = (uint16_t)(f->id >> 8);
  const uint8_t *d = f->data;
  if (type == 1U)
  {
    t->position = SignedRange(ReadBE16(d), COMMAND_P);
    t->velocity = SignedRange(ReadBE16(d+2), COMMAND_V);
    t->kp = ReadBE16(d+4) * (500.0f / 65535.0f);
    t->kd = ReadBE16(d+6) * (5.0f / 65535.0f);
    t->torque = SignedRange(field, COMMAND_T);
    return CMD_MOTION;
  }
  if (field != MASTER) { return CMD_IGNORE; }
  switch (type)
  {
    case 0: return BytesZero(d, 8) ? CMD_READ : CMD_IGNORE;
    case 3: return BytesZero(d, 8) ? CMD_ENABLE : CMD_IGNORE;
    case 4:
      if (!BytesZero(d+1, 7) || d[0] > 1U) { return CMD_IGNORE; }
      return d[0] ? CMD_CLEAR : CMD_STOP;
    case 6: return d[0] == 1U && BytesZero(d+1, 7) ? CMD_RAM_ZERO : CMD_IGNORE;
    case 17:
      if (!BytesZero(d+2, 6)) { return CMD_IGNORE; }
      switch (ReadLE16(d))
      {
        case 0x7005: case 0x7019: case 0x701B: case 0x701C: return CMD_READ;
        default: return CMD_IGNORE;
      }
    default: return CMD_IGNORE;
  }
}

uint8_t ProfileReply(uint8_t node, const CanFrame_t *f, const ProfileState *s, CanFrame_t *out)
{
  const unsigned type = f->id >> 24;
  out->extended = 1;
  out->dlc = 8;
  out->id = ((uint32_t)node << 8) | MASTER;
  if (type == 0)
  {
    out->id = ((uint32_t)node << 8) | 0xFEU;
    memcpy(out->data, s->uid, 8);
  }
  else if (type == 17)
  {
    out->id |= 17UL << 24;
    uint16_t index = ReadLE16(f->data);
    WriteLE16(out->data, index);
    float value = 0; /* run_mode = 0 (motion), remaining bytes zero */
    if (index == 0x7019) { value = s->position; }
    if (index == 0x701B) { value = s->velocity; }
    if (index == 0x701C) { value = s->vbus; }
    _Static_assert(sizeof(float) == 4, "parameter values require binary32");
    uint32_t bits;
    memcpy(&bits, &value, 4);
    WriteLE16(out->data+4, (uint16_t)bits);
    WriteLE16(out->data+6, (uint16_t)(bits >> 16));
  }
  else
  {
    uint32_t fault = 0;
    if (s->fault & SERVO_FAULT_UNDERVOLTAGE) { fault |= 1U; }
    if (s->fault & SERVO_FAULT_OVERCURRENT) { fault |= 2U; }
    if (s->fault & SERVO_FAULT_OVER_TEMP) { fault |= 4U; }
    if (s->fault & SERVO_FAULT_ENCODER) { fault |= 8U; }
    if (!s->calibrated) { fault |= 32U; }
    out->id |= (2UL << 24) | ((s->enabled && !s->fault ? 2UL : 0UL) << 22) | (fault << 16);
    WriteBE16(out->data, PackRange(s->position, 2.0f * RAD_PER_TURN));
    WriteBE16(out->data+2, PackRange(s->velocity, FEEDBACK_V));
    WriteBE16(out->data+4, PackRange(s->torque, FEEDBACK_T));
    WriteBE16(out->data+6, ScaledI16(s->temperature * 10.0f));
  }
  return 1;
}
