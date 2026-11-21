#include "node.h"
#include "adc.h"

#include "board.h"
#include "cali_nv.h"
#include "can.h"
#include "config.h"
#include "debug.h"
#include "foc_math.h"
#include "pwm.h"
#include "servo.h"

#define NODE_DEBOUNCE_MS       (30U)
#define NODE_LONG_MS           (2000U)
#define NODE_IDLE_MS           (10000U)
#define NODE_RESET_DELAY_MS    (20U)

#define NODE_FAST_MS           (200U)
#define NODE_LONG_ON_MS        (400U)
#define NODE_LONG_OFF_MS       (200U)
#define NODE_SHORT_ON_MS       (120U)
#define NODE_SHORT_OFF_MS      (120U)
#define NODE_GAP_MS            (400U)
#define NODE_PAUSE_MS          (1200U)

typedef enum
{
  NODE_BTN_IDLE = 0,
  NODE_BTN_DOWN,
  NODE_BTN_EDIT
} NodeBtnState_t;

typedef enum
{
  LED_PAUSE = 0,
  LED_TENS_ON,
  LED_TENS_OFF,
  LED_GAP,
  LED_ONES_ON,
  LED_ONES_OFF
} NodeLedPhase_t;

static uint8_t s_id;
static uint8_t s_edit;
static uint8_t s_edit_id;
static uint8_t s_reset_pending;
static uint32_t s_reset_at;

static NodeBtnState_t s_btn_st;
static uint8_t s_btn_stable;
static uint8_t s_btn_last;
static uint32_t s_btn_edge_ms;
static uint32_t s_btn_down_ms;
static uint32_t s_edit_last_ms;
static uint8_t s_long_fired;

static NodeLedPhase_t s_led_ph;
static uint8_t s_led_left;
static uint32_t s_led_next_ms;
static uint8_t s_led_shown_id;
static uint8_t s_led_fault_on;
static uint32_t s_led_fault_ms;

static uint8_t Node_ClampId(uint8_t id)
{
  if ((id < CFG_NODE_ID_MIN) || (id > CFG_NODE_ID_MAX))
  {
    return (uint8_t)CFG_NODE_ID;
  }
  return id;
}

static uint8_t Node_IncId(uint8_t id)
{
  if (id >= CFG_NODE_ID_MAX)
  {
    return (uint8_t)CFG_NODE_ID_MIN;
  }
  return (uint8_t)(id + 1U);
}

static void Node_LedRestart(uint8_t id)
{
  s_led_shown_id = id;
  s_led_ph = LED_PAUSE;
  s_led_left = 0U;
  s_led_next_ms = Board_GetTickMs();
  Board_LedSet(0U);
}

static void Node_LedPattern(uint32_t now)
{
  const uint8_t id = s_edit != 0U ? s_edit_id : s_id;
  const uint8_t tens = (uint8_t)(id / 10U);
  const uint8_t ones = (uint8_t)(id % 10U);

  if (id != s_led_shown_id)
  {
    Node_LedRestart(id);
  }
  if ((int32_t)(now - s_led_next_ms) < 0)
  {
    return;
  }

  switch (s_led_ph)
  {
    case LED_PAUSE:
      Board_LedSet(0U);
      if (tens > 0U)
      {
        s_led_ph = LED_TENS_ON;
        s_led_left = tens;
        s_led_next_ms = now;
      }
      else if (ones > 0U)
      {
        s_led_ph = LED_ONES_ON;
        s_led_left = ones;
        s_led_next_ms = now;
      }
      else
      {
        s_led_next_ms = now + NODE_PAUSE_MS;
      }
      break;

    case LED_TENS_ON:
      Board_LedSet(1U);
      s_led_ph = LED_TENS_OFF;
      s_led_next_ms = now + NODE_LONG_ON_MS;
      break;

    case LED_TENS_OFF:
      Board_LedSet(0U);
      s_led_left--;
      if (s_led_left > 0U)
      {
        s_led_ph = LED_TENS_ON;
        s_led_next_ms = now + NODE_LONG_OFF_MS;
      }
      else if (ones > 0U)
      {
        s_led_ph = LED_GAP;
        s_led_next_ms = now + NODE_GAP_MS;
      }
      else
      {
        s_led_ph = LED_PAUSE;
        s_led_next_ms = now + NODE_PAUSE_MS;
      }
      break;

    case LED_GAP:
      Board_LedSet(0U);
      s_led_ph = LED_ONES_ON;
      s_led_left = ones;
      s_led_next_ms = now;
      break;

    case LED_ONES_ON:
      Board_LedSet(1U);
      s_led_ph = LED_ONES_OFF;
      s_led_next_ms = now + NODE_SHORT_ON_MS;
      break;

    case LED_ONES_OFF:
      Board_LedSet(0U);
      s_led_left--;
      if (s_led_left > 0U)
      {
        s_led_ph = LED_ONES_ON;
        s_led_next_ms = now + NODE_SHORT_OFF_MS;
      }
      else
      {
        s_led_ph = LED_PAUSE;
        s_led_next_ms = now + NODE_PAUSE_MS;
      }
      break;

    default:
      s_led_ph = LED_PAUSE;
      s_led_next_ms = now + NODE_PAUSE_MS;
      break;
  }
}

static void Node_EnterEdit(uint32_t now)
{
  s_edit = 1U;
  s_edit_id = s_id;
  s_edit_last_ms = now;
  s_btn_st = NODE_BTN_EDIT;
  Servo_HoldPosition();
  Node_LedRestart(s_edit_id);
  Dbg_Printf("node: edit id=%d\r\n", (int)s_edit_id);
}

static void Node_ExitEdit(void)
{
  s_edit = 0U;
  s_btn_st = NODE_BTN_IDLE;
  Node_LedRestart(s_id);
}

static void Node_ButtonService(uint32_t now)
{
  const uint8_t raw = Board_ButtonRaw();

  if (raw != s_btn_last)
  {
    s_btn_last = raw;
    s_btn_edge_ms = now;
  }
  if (((now - s_btn_edge_ms) >= NODE_DEBOUNCE_MS) && (raw != s_btn_stable))
  {
    s_btn_stable = raw;
    if (raw != 0U)
    {
      s_btn_down_ms = now;
      s_long_fired = 0U;
      if (s_btn_st == NODE_BTN_IDLE)
      {
        s_btn_st = NODE_BTN_DOWN;
      }
    }
    else if (s_long_fired == 0U)
    {
      if ((s_btn_st == NODE_BTN_DOWN) && (s_edit == 0U))
      {
        s_btn_st = NODE_BTN_IDLE;
      }
      else if (s_edit != 0U)
      {
        s_edit_id = Node_IncId(s_edit_id);
        s_edit_last_ms = now;
        Node_LedRestart(s_edit_id);
        Dbg_Printf("node: cand=%d\r\n", (int)s_edit_id);
      }
    }
    else
    {
      s_long_fired = 0U;
    }
  }

  if ((s_btn_stable != 0U) && (s_long_fired == 0U) &&
      ((now - s_btn_down_ms) >= NODE_LONG_MS))
  {
    s_long_fired = 1U;
    if (s_edit == 0U)
    {
      Node_EnterEdit(now);
    }
    else
    {
      Dbg_Printf("node: save id=%d\r\n", (int)s_edit_id);
      if (Node_ApplyAndReset(s_edit_id) == 0U)
      {
        Dbg_Printf("node: save fail\r\n");
        Node_ExitEdit();
      }
    }
  }

  if ((s_edit != 0U) && (s_btn_stable == 0U) &&
      ((now - s_edit_last_ms) >= NODE_IDLE_MS))
  {
    Dbg_Printf("node: edit timeout\r\n");
    Node_ExitEdit();
  }
}

void Node_Init(void)
{
  const uint8_t stored = CaliNv_LoadNodeId();

  s_id = Node_ClampId(stored != 0U ? stored : (uint8_t)CFG_NODE_ID);
  s_edit = 0U;
  s_edit_id = s_id;
  s_reset_pending = 0U;
  s_btn_st = NODE_BTN_IDLE;
  s_btn_stable = 0U;
  s_btn_last = Board_ButtonRaw();
  s_btn_edge_ms = Board_GetTickMs();
  Node_LedRestart(s_id);
  Dbg_Printf("node: id=%d\r\n", (int)s_id);
}

uint8_t Node_GetId(void)
{
  return s_id;
}

uint8_t Node_ApplyAndReset(uint8_t new_id)
{
  if ((new_id < CFG_NODE_ID_MIN) || (new_id > CFG_NODE_ID_MAX))
  {
    return 0U;
  }
  if (new_id == s_id)
  {
    Node_ExitEdit();
    return 1U;
  }

  const ServoMode_t previous = Servo_PauseForFlash();
  Can_StopForFlash();
  if (CaliNv_SaveNodeId(new_id) == 0U)
  {
    Can_Restart();
    Adc_RefreshVbus();
    Servo_ResumeAfterFlash(previous);
    return 0U;
  }
  Can_Restart();

  s_reset_pending = 1U;
  s_reset_at = Board_GetTickMs() + NODE_RESET_DELAY_MS;
  return 1U;
}

void Node_Service(uint8_t cali_ok)
{
  const uint32_t now = Board_GetTickMs();

  if (s_reset_pending != 0U)
  {
    if ((int32_t)(now - s_reset_at) >= 0)
    {
      Board_SystemReset();
    }
    return;
  }

  Node_ButtonService(now);

  if ((cali_ok == 0U) && (s_edit == 0U))
  {
    if ((now - s_led_fault_ms) >= NODE_FAST_MS)
    {
      s_led_fault_ms = now;
      s_led_fault_on = (s_led_fault_on == 0U) ? 1U : 0U;
      Board_LedSet(s_led_fault_on);
    }
    return;
  }

  Node_LedPattern(now);
}
