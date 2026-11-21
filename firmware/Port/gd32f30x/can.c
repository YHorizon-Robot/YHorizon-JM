#include "can.h"
#include "multi_axis.h"
#include "board.h"
#include "config.h"
#include "gd32f30x.h"

static CanFrame_t s_rx[CFG_CAN_RX_SLOTS];
static volatile uint8_t s_rx_head;
static volatile uint8_t s_rx_tail;
static volatile uint8_t s_bus_off_pending;
static uint8_t s_node_id = 1U;

static void Can_ConfigFilters(uint8_t id)
{
  static const uint16_t bases[5] = {
    CFG_CAN_MOTION_BASE,
    CFG_CAN_VEL_BASE,
    CFG_CAN_GAINS_BASE,
    CFG_CAN_POS_BASE,
    CFG_CAN_MGMT_BASE
  };
  can_filter_parameter_struct filter;
  uint32_t i;

  can_struct_para_init(CAN_FILTER_STRUCT, &filter);
  filter.filter_mode = CAN_FILTERMODE_MASK;
  filter.filter_bits = CAN_FILTERBITS_32BIT;
  filter.filter_fifo_number = CAN_FIFO0;
  filter.filter_enable = ENABLE;
  filter.filter_list_low = 0U;
  filter.filter_mask_low = 0x0006U;
  filter.filter_mask_high = (uint16_t)(0x7FFU << 5);

  for (i = 0U; i < 5U; i++)
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

    can_message_receive(CAN0, CAN_FIFO0, &rx_msg);
    if ((rx_msg.rx_ff != CAN_FF_STANDARD) ||
        (rx_msg.rx_ft != CAN_FT_DATA) ||
        (rx_msg.rx_dlen != 8U))
    {
      continue;
    }

    if (rx_msg.rx_sfid == CFG_CAN_SYNC_ID) { MultiAxis_OnSyncIrq(rx_msg.rx_data); continue; }
    next = (uint8_t)((s_rx_head + 1U) % CFG_CAN_RX_SLOTS);
    if (next == s_rx_tail)
    {
      continue;
    }

    s_rx[s_rx_head].id = rx_msg.rx_sfid;
    for (i = 0U; i < 8U; i++)
    {
      s_rx[s_rx_head].data[i] = rx_msg.rx_data[i];
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
  nvic_irq_disable(USBD_LP_CAN0_RX0_IRQn);
  nvic_irq_disable(CAN0_EWMC_IRQn);
  can_interrupt_disable(CAN0, CAN_INT_RFNE0);
  can_interrupt_disable(CAN0, CAN_INT_ERR);
  can_interrupt_disable(CAN0, CAN_INT_BO);
  (void)can_working_mode_set(CAN0, CAN_MODE_INITIALIZE);
}

void Can_Restart(void)
{
  can_receive_message_struct rx_msg;

  s_rx_head = 0U;
  s_rx_tail = 0U;
  s_bus_off_pending = 0U;

  Can_HwStart(s_node_id);
  while (can_receive_message_length_get(CAN0, CAN_FIFO0) != 0U)
  {
    can_message_receive(CAN0, CAN_FIFO0, &rx_msg);
  }
}

void Can_Service(void)
{
  if ((s_bus_off_pending == 0U) && (RESET == can_flag_get(CAN0, CAN_FLAG_BOERR)))
  {
    return;
  }
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
  for (i = 0U; i < 8U; i++)
  {
    frame->data[i] = s_rx[tail].data[i];
  }
  s_rx_tail = (uint8_t)((tail + 1U) % CFG_CAN_RX_SLOTS);
  return 1U;
}

uint8_t Can_Send(uint32_t id, const uint8_t data[8])
{
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
}
