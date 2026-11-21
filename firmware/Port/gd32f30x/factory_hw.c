#include "factory_hw.h"

#include "board.h"
#include "config.h"
#include "gd32f30x.h"

void FactoryHw_UseInternalClock(void)
{
  SystemCoreClock = IRC8M_VALUE;
  (void)SysTick_Config(SystemCoreClock / 1000U);
}

uint8_t FactoryHw_CanLoopback(void)
{
  can_parameter_struct can_param;
  can_filter_parameter_struct filter;
  can_transmit_message_struct tx;
  can_receive_message_struct rx;
  uint8_t mailbox;
  uint8_t i;
  uint32_t t0;

  can_deinit(CAN0);
  can_struct_para_init(CAN_INIT_STRUCT, &can_param);
  can_param.working_mode = CAN_SILENT_LOOPBACK_MODE;
  can_param.resync_jump_width = CAN_BT_SJW_1TQ;
  can_param.time_segment_1 = (uint8_t)(CFG_CAN_BS1_TQ - 1U);
  can_param.time_segment_2 = (uint8_t)(CFG_CAN_BS2_TQ - 1U);
  can_param.time_triggered = DISABLE;
  can_param.auto_bus_off_recovery = ENABLE;
  can_param.auto_wake_up = DISABLE;
  can_param.auto_retrans = ENABLE;
  can_param.rec_fifo_overwrite = DISABLE;
  can_param.trans_fifo_order = DISABLE;
  can_param.prescaler = CFG_CAN_PRESCALER;
  if (SUCCESS != can_init(CAN0, &can_param))
  {
    return 1U;
  }

  can_struct_para_init(CAN_FILTER_STRUCT, &filter);
  filter.filter_number = 0U;
  filter.filter_mode = CAN_FILTERMODE_MASK;
  filter.filter_bits = CAN_FILTERBITS_32BIT;
  filter.filter_list_high = 0U;
  filter.filter_list_low = 0U;
  filter.filter_mask_high = 0U;
  filter.filter_mask_low = 0U;
  filter.filter_fifo_number = CAN_FIFO0;
  filter.filter_enable = ENABLE;
  can_filter_init(&filter);

  tx.tx_sfid = 0x123U;
  tx.tx_efid = 0U;
  tx.tx_ff = CAN_FF_STANDARD;
  tx.tx_ft = CAN_FT_DATA;
  tx.tx_dlen = 8U;
  for (i = 0U; i < 8U; i++)
  {
    tx.tx_data[i] = (uint8_t)(0xA5U + i);
  }

  mailbox = can_message_transmit(CAN0, &tx);
  if (mailbox == CAN_NOMAILBOX)
  {
    can_deinit(CAN0);
    return 2U;
  }

  t0 = Board_GetTickMs();
  while ((CAN_TRANSMIT_OK != can_transmit_states(CAN0, mailbox)) &&
         ((Board_GetTickMs() - t0) < 10U))
  {
  }
  if ((CAN_TRANSMIT_OK != can_transmit_states(CAN0, mailbox)) ||
      (can_receive_message_length_get(CAN0, CAN_FIFO0) == 0U))
  {
    can_deinit(CAN0);
    return 3U;
  }

  can_message_receive(CAN0, CAN_FIFO0, &rx);
  can_deinit(CAN0);
  if ((rx.rx_sfid != 0x123U) || (rx.rx_data[0] != 0xA5U) || (rx.rx_dlen != 8U))
  {
    return 4U;
  }
  return 0U;
}
