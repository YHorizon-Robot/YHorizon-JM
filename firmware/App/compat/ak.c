#include "profile.h"
#include "mit_wire.h"
#include "servo.h"

ProfileAction ProfileDecode(uint8_t node, const CanFrame_t *f, ProfileTarget *t)
{
  (void)node;
  static const uint8_t prefix[7] = {255, 255, 255, 255, 255, 255, 255};
  if (memcmp(f->data, prefix, sizeof(prefix)) == 0)
  {
    if (f->data[7] == 0xFC) { return CMD_ENABLE; }
    if (f->data[7] == 0xFD) { return CMD_STOP; }
    return CMD_IGNORE;
  }
  MitMotion m;
  Mit_Unpack(f->data, 45.0f, 15.0f, &m);
  t->position = m.position; t->velocity = m.velocity;
  t->kp = m.kp; t->kd = m.kd; t->torque = m.torque;
  return CMD_MOTION;
}

uint8_t ProfileReply(uint8_t node, const CanFrame_t *f, const ProfileState *s, CanFrame_t *out)
{
  (void)f;
  out->id = node;
  out->dlc = 8;
  Mit_PackMeasurements(out->data, 45.0f, 15.0f, s->position, s->velocity, s->torque);
  out->data[6] = (uint8_t)fminf(255.0f, fmaxf(0.0f, s->temperature + 40.0f));
  out->data[0] = node;
  if (s->fault & SERVO_FAULT_OVER_TEMP) { out->data[7] = 6; }
  else if (s->fault & SERVO_FAULT_OVERCURRENT) { out->data[7] = 2; }
  else if (s->fault & SERVO_FAULT_UNDERVOLTAGE) { out->data[7] = 4; }
  else if (s->fault & SERVO_FAULT_ENCODER) { out->data[7] = 5; }
  else if (s->fault) { return 0; } /* no equivalent AK error code */
  return 1;
}
