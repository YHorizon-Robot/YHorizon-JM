#ifndef PWM_H
#define PWM_H

#ifdef __cplusplus
extern "C" {
#endif

void Pwm_StartTimer(void);
void Pwm_EnableOutputs(void);
void Pwm_DisableOutputs(void);
void Pwm_ApplyDuty(float duty_a, float duty_b, float duty_c);
/** Commit before the next valley when possible, otherwise queue the entire triplet. */
void Pwm_ApplyDutyEarly(float duty_a, float duty_b, float duty_c);
/** Commit a queued triplet after the valley update event. */
void Pwm_ApplyDeferredDuty(void);
void CtrlTimer_Start(void);

#ifdef __cplusplus
}
#endif

#endif /* PWM_H */
