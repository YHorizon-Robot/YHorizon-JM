#include "adc.h"
#include "board.h"
#include "cali.h"
#include "can.h"
#include "comm.h"
#include "config.h"
#include "debug.h"
#include "encoder.h"
#include "node.h"
#include "pwm.h"
#include "servo.h"

int main(void)
{
  uint32_t last_dbg_ms = 0U;
  uint8_t cali_ok;

  Board_Init();
  Dbg_Init();
  Encoder_Init();
  Servo_Init();
  Node_Init();
  Can_Init(Node_GetId());

  Dbg_Printf("YHorizon-JM GD32F303 current servo\r\n");
  Pwm_StartTimer();
  Board_LedSet(1U);
  cali_ok = Cali_Start();
  Comm_Init(cali_ok);
  if (cali_ok == 0U)
  {
    Dbg_Printf("cali failed\r\n");
  }
  else
  {
    Dbg_Printf("run\r\n");
  }

  for (;;)
  {
    uint32_t now;

    Can_Service();
    Adc_Service();
    Comm_Process();
    Node_Service(cali_ok);

    now = HAL_GetTick();
    if ((now - last_dbg_ms) >= CFG_DEBUG_PERIOD_MS)
    {
      ServoTelemetry_t tel;
      last_dbg_ms = now;
      Servo_GetTelemetry(&tel);
      Dbg_Printf("m=%d il=%d p=%d v=%d t=%d iq=%d vbus=%d\r\n",
                 (int)tel.ctrl_mode,
                 (int)tel.inner_loop,
                 (int)(tel.p_act * 1000.0f),
                 (int)(tel.v_act * 1000.0f),
                 (int)(tel.t_ref * 1000.0f),
                 (int)(tel.iq * 100.0f),
                 (int)(Adc_GetVbusVolts() * 100.0f));
    }
  }
}
