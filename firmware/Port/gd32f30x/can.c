#include "can.h"
#include "multi_axis.h"
#include "board.h"
#include "config.h"
#include "gd32f30x.h"
#if CFG_CAN_COMPAT
#include "comm.h"
#if CFG_DM4310
#include "compat/dm4310.h"
#else
#include "compat/profile.h"
#endif
static volatile uint8_t s_rx_lost;
static uint32_t s_rx_losses;
static uint32_t s_tx_drops;
#endif

static CanFrame_t s_rx[CFG_CAN_RX_SLOTS];
static volatile uint8_t s_rx_head;
static volatile uint8_t s_rx_tail;
static volatile uint8_t s_bus_off_pending;
static uint8_t s_node_id = 1U;

static void Can_ConfigFilters(uint8_t id)
{
#if CFG_DM4310
  /* Both are exact standard DATA filters. Register requests carry the node in D0/D1. */
  const uint16_t ids[] = {id, DM4310_REGISTER_ID};
  if (id < 1U || id > DM4310_MAX_NODE) { return; }
#elif CFG_CAN_COMPAT
  if (!ProfileNodeValid(id)) { return; }
#else
  static const uint16_t bases[5] = {
    CFG_CAN_MOTION_BASE,
    CFG_CAN_VEL_BASE,
    CFG_CAN_GAINS_BASE,
    CFG_CAN_POS_BASE,
    CFG_CAN_MGMT_BASE
  };
#endif
  can_filter_parameter_struct filter;

  can_struct_para_init(CAN_FILTER_STRUCT, &filter);
  filter.filter_mode = CAN_FILTERMODE_MASK;
  filter.filter_bits = CAN_FILTERBITS_32BIT;
  filter.filter_fifo_number = CAN_FIFO0;
  filter.filter_enable = ENABLE;
  filter.filter_list_low = 0U;
  filter.filter_mask_low = 0x0006U;
  filter.filter_mask_high = (uint16_t)(0x7FFU << 5);

#if CFG_CAN_COMPAT && !CFG_DM4310
  const unsigned shift = PROFILE_EXT ? 3U : 21U;
  const uint32_t key = (ProfileId(id) << shift) | (PROFILE_EXT ? 4U : 0U);
  const uint32_t mask = (PROFILE_MASK << shift) | 6U;
  filter.filter_number = 0;
  filter.filter_list_low = (uint16_t)key;
  filter.filter_list_high = (uint16_t)(key >> 16);
  filter.filter_mask_low = (uint16_t)mask;
  filter.filter_mask_high = (uint16_t)(mask >> 16);
  can_filter_init(&filter);
#elif CFG_DM4310
  for (uint32_t i = 0U; i < sizeof(ids) / sizeof(ids[0]); i++)
  {
    filter.filter_number = (uint16_t)i;
    filter.filter_list_high = (uint16_t)(ids[i] << 5);
    can_filter_init(&filter);
  }
#else
  for (uint32_t i = 0U; i < 5U; i++)
  {
    const uint16_t sid = (uint16_t)(bases[i] + (uint16_t)id);
    filter.filter_number = (uint16_t)i;
    filter.filter_list_high = (uint16_t)(sid << 5);
    can_filter_init(&filter);
  }
  /** @brief 节点号 0 的管理帧是广播，只接受 DISABLE。 */
  filter.filter_number = 5U;
  filter.filter_list_high = (uint16_t)(CFG_CAN_BROADCAST_ID << 5);
  can_filter_init(&filter);
  filter.filter_number = 6U;
  filter.filter_list_high = (uint16_t)(CFG_CAN_SYNC_ID << 5);
  can_filter_init(&filter);
#endif
}

static void Can_HwStart(uint8_t node_id)
{
  can_parameter_struct can_param;

  s_node_id = ((node_id >= 1U) && (node_id <= 63U)) ? node_id : 1U;

  can_deinit(CAN0);
  can_struct_para_init(CAN_INIT_STRUCT, &can_param);
  can_param.working_mode = CAN_NORMAL_MODE;
  can_param.resync_jump_width = CAN_BT_SJW_1TQ;
  can_param.time_segment_1 = (uint8_t)(CFG_CAN_BS1_TQ - 1U);
  can_param.time_segment_2 = (uint8_t)(CFG_CAN_BS2_TQ - 1U);
  can_param.time_triggered = DISABLE;
  can_param.auto_bus_off_recovery = ENABLE;
  can_param.auto_wake_up = DISABLE;
  can_param.auto_retrans = ENABLE;
  can_param.rec_fifo_overwrite = DISABLE;
  can_param.trans_fifo_order = DISABLE;
  /* APB1 / (prescaler * (1 + BS1 + BS2)) = 1 Mbps. BS1/BS2 寄存器值比 TQ 数少 1。 */
  can_param.prescaler = CFG_CAN_PRESCALER;
  _Static_assert((CFG_CAN_BS1_TQ >= 1U) && (CFG_CAN_BS1_TQ <= 16U), "CAN BS1 out of range");
  _Static_assert((BOARD_APB1_HZ % (CFG_CAN_PRESCALER * CFG_CAN_BITRATE_HZ)) == 0U, "CAN bitrate not integer");
  if (SUCCESS != can_init(CAN0, &can_param))
  {
    Error_Handler();
  }

  Can_ConfigFilters(s_node_id);

  can_interrupt_enable(CAN0, CAN_INT_RFNE0);
  can_interrupt_enable(CAN0, CAN_INT_ERR);
  can_interrupt_enable(CAN0, CAN_INT_BO);
  nvic_irq_enable(USBD_LP_CAN0_RX0_IRQn, CFG_NVIC_CAN, 0U);
  nvic_irq_enable(CAN0_EWMC_IRQn, CFG_NVIC_CAN, 0U);
}

void Can_Init(uint8_t node_id)
{
  s_rx_head = 0U;
  s_rx_tail = 0U;
  s_bus_off_pending = 0U;
  Can_HwStart(node_id);
}

void Can_ProcessRxIrq(void)
{
  can_receive_message_struct rx_msg;

  while (can_receive_message_length_get(CAN0, CAN_FIFO0) != 0U)
  {
    uint8_t next;
    uint8_t i;

#if CFG_CAN_COMPAT
    /* The SPL dequeues with a read/modify/write on RFIFO0 (overrun is W1C).
     * Treat a full hardware FIFO as loss before that flag can be cleared. */
    if (can_receive_message_length_get(CAN0, CAN_FIFO0) == 3U ||
        can_flag_get(CAN0, CAN_FLAG_RFO0) != RESET) { s_rx_lost = 1U; }
#endif
    can_message_receive(CAN0, CAN_FIFO0, &rx_msg);
#if CFG_CAN_COMPAT && !CFG_DM4310
    const uint8_t extended = rx_msg.rx_ff == CAN_FF_EXTENDED;
    const uint32_t id = extended ? rx_msg.rx_efid : rx_msg.rx_sfid;
    if (rx_msg.rx_ft != CAN_FT_DATA || !ProfileAccepts(s_node_id, id, extended, rx_msg.rx_dlen))
    { continue; }
#else
    if ((rx_msg.rx_ff != CAN_FF_STANDARD) ||
        (rx_msg.rx_ft != CAN_FT_DATA) ||
        (rx_msg.rx_dlen != 8U))
    {
      continue;
    }

#endif
#if !CFG_CAN_COMPAT
    if (rx_msg.rx_sfid == CFG_CAN_SYNC_ID) { MultiAxis_OnSyncIrq(rx_msg.rx_data); continue; }
#endif
    next = (uint8_t)((s_rx_head + 1U) % CFG_CAN_RX_SLOTS);
    if (next == s_rx_tail)
    {
#if CFG_CAN_COMPAT
      s_rx_lost = 1U;
#endif
      continue;
    }

#if CFG_CAN_COMPAT && !CFG_DM4310
    s_rx[s_rx_head].id = id;
#else
    s_rx[s_rx_head].id = rx_msg.rx_sfid;
#endif
#if CFG_CAN_COMPAT
    s_rx[s_rx_head].extended = rx_msg.rx_ff == CAN_FF_EXTENDED;
    s_rx[s_rx_head].dlc = rx_msg.rx_dlen;
    s_rx[s_rx_head].received_ms = Board_GetTickMs();
#endif
    for (i = 0U; i < 8U; i++)
    {
#if CFG_CAN_COMPAT && !CFG_DM4310
      s_rx[s_rx_head].data[i] = i < rx_msg.rx_dlen ? rx_msg.rx_data[i] : 0U;
#else
      s_rx[s_rx_head].data[i] = rx_msg.rx_data[i];
#endif
    }
    s_rx_head = next;
  }
}

void Can_OnBusOffIrq(void)
{
  if (SET == can_interrupt_flag_get(CAN0, CAN_INT_FLAG_BOERR))
  {
    can_interrupt_flag_clear(CAN0, CAN_INT_FLAG_BOERR);
    s_bus_off_pending = 1U;
  }
  if (SET == can_interrupt_flag_get(CAN0, CAN_INT_FLAG_ERRIF))
  {
    can_interrupt_flag_clear(CAN0, CAN_INT_FLAG_ERRIF);
  }
}

void Can_StopForFlash(void)
{
#if CFG_CAN_COMPAT
  Comm_OnCanLoss();
#endif
  nvic_irq_disable(USBD_LP_CAN0_RX0_IRQn);
  nvic_irq_disable(CAN0_EWMC_IRQn);
  can_interrupt_disable(CAN0, CAN_INT_RFNE0);
  can_interrupt_disable(CAN0, CAN_INT_ERR);
  can_interrupt_disable(CAN0, CAN_INT_BO);
  (void)can_working_mode_set(CAN0, CAN_MODE_INITIALIZE);
}

void Can_Restart(void)
{
#if !CFG_CAN_COMPAT
  can_receive_message_struct rx_msg;
#else
  s_rx_lost = 0U;
#endif

  s_rx_head = 0U;
  s_rx_tail = 0U;
  s_bus_off_pending = 0U;

  Can_HwStart(s_node_id);
#if !CFG_CAN_COMPAT
  while (can_receive_message_length_get(CAN0, CAN_FIFO0) != 0U)
  {
    can_message_receive(CAN0, CAN_FIFO0, &rx_msg);
  }
#endif
}

#if CFG_CAN_COMPAT
uint8_t Can_RxLost(void)
{
  return s_rx_lost || s_bus_off_pending || can_flag_get(CAN0, CAN_FLAG_BOERR) != RESET;
}

void Can_GetLossCounts(uint32_t *rx, uint32_t *tx)
{
  *rx = s_rx_losses;
  *tx = s_tx_drops;
}
#endif

void Can_Service(void)
{
#if CFG_CAN_COMPAT
  if (!Can_RxLost())
#else
  if ((s_bus_off_pending == 0U) && (RESET == can_flag_get(CAN0, CAN_FLAG_BOERR)))
#endif
  {
    return;
  }
#if CFG_CAN_COMPAT
  ++s_rx_losses; /* Count recovery episodes, not individual lost frames. */
#endif
  Can_StopForFlash();
  Can_Restart();
}

uint8_t Can_PopRx(CanFrame_t *frame)
{
  uint8_t i;
  uint8_t tail;

  if (frame == 0)
  {
    return 0U;
  }

  tail = s_rx_tail;
  if (tail == s_rx_head)
  {
    return 0U;
  }

  frame->id = s_rx[tail].id;
#if CFG_CAN_COMPAT
  frame->extended = s_rx[tail].extended;
  frame->dlc = s_rx[tail].dlc;
  frame->received_ms = s_rx[tail].received_ms;
#endif
  for (i = 0U; i < 8U; i++)
  {
    frame->data[i] = s_rx[tail].data[i];
  }
  s_rx_tail = (uint8_t)((tail + 1U) % CFG_CAN_RX_SLOTS);
  return 1U;
}

#if CFG_CAN_COMPAT
uint8_t Can_SendFrame(const CanFrame_t *frame)
{
  can_transmit_message_struct tx = {0};
  if (frame->dlc > 8U || frame->id > (frame->extended ? 0x1FFFFFFFU : 0x7FFU))
  { return 0U; }
  tx.tx_ff = frame->extended ? CAN_FF_EXTENDED : CAN_FF_STANDARD;
  tx.tx_sfid = frame->extended ? 0U : frame->id;
  tx.tx_efid = frame->extended ? frame->id : 0U;
  tx.tx_ft = CAN_FT_DATA;
  tx.tx_dlen = frame->dlc;
  for (unsigned i = 0; i < frame->dlc; ++i) { tx.tx_data[i] = frame->data[i]; }
  if (can_message_transmit(CAN0, &tx) == CAN_NOMAILBOX) { ++s_tx_drops; return 0U; }
  return 1U;
}
#endif

uint8_t Can_Send(uint32_t id, const uint8_t data[8])
{
#if CFG_CAN_COMPAT
  CanFrame_t frame = {.id = id, .dlc = 8U};
  for (unsigned i = 0; i < 8; ++i) { frame.data[i] = data[i]; }
  return Can_SendFrame(&frame);
#else
  can_transmit_message_struct tx_msg;
  uint8_t mailbox;
  uint8_t i;

  tx_msg.tx_sfid = id;
  tx_msg.tx_efid = 0U;
  tx_msg.tx_ff = CAN_FF_STANDARD;
  tx_msg.tx_ft = CAN_FT_DATA;
  tx_msg.tx_dlen = 8U;
  for (i = 0U; i < 8U; i++)
  {
    tx_msg.tx_data[i] = data[i];
  }

  mailbox = can_message_transmit(CAN0, &tx_msg);
  return (mailbox != CAN_NOMAILBOX) ? 1U : 0U;
#endif
}
