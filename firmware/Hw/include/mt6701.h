#ifndef MT6701_H
#define MT6701_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief MT6701 单圈计数。 */
#define MT6701_CPR           (16384U)
/**
 * @brief 全零帧只有在上一次有效角度离零点不超过这么多计数时才当作角度 0。
 *
 * 20 kHz 采样下相当于约 4700 rpm 的单周期位移。转得更快时零点这一格本来就很少被采到，
 * 丢一帧由预测补上。
 */
#define MT6701_ZERO_WINDOW   (64U)

/**
 * @brief 一路 MT6701 的解析上下文，记住上一次有效角度。
 */
typedef struct
{
  uint16_t last_raw;
  uint8_t have_last;
} Mt6701_Link_t;

/**
 * @brief  MT6701 SSI 的 6 位 CRC，多项式 x^6 + x + 1，初值 0。
 * @param  data18  14 位角度加 4 位状态。
 * @return 6 位校验值。
 */
static inline uint8_t Mt6701_CalcCrc6(uint32_t data18)
{
  uint8_t crc = 0U;
  int8_t bit;

  for (bit = 17; bit >= 0; bit--)
  {
    const uint8_t in = (uint8_t)((data18 >> (uint8_t)bit) & 0x1U);
    const uint8_t msb = (uint8_t)((crc >> 5U) & 0x1U);
    crc = (uint8_t)((crc << 1U) & 0x3FU);
    if ((msb ^ in) != 0U)
    {
      crc ^= 0x03U;
    }
  }
  return (uint8_t)(crc & 0x3FU);
}

/**
 * @brief  解析一帧 24 位 SSI 数据。
 *
 * 角度 0、状态 0 的合法帧校验值也是 0，线上就是 `00 00 00`。上电约 30 ms 内的无效帧
 * 和数据线卡低同样读成全零，只能靠上一次有效角度区分：紧邻零点才接受。
 * 数据线恰好在零点附近卡低时无法识别，这是剩下的盲区。
 *
 * @param[in,out] link        本路编码器的上下文。
 * @param[in]     raw         先收到的字节在前的 3 字节。
 * @param[out]    angle_out   14 位角度。
 * @param[out]    status_out  4 位状态，可为空。
 * @return 1 表示帧有效。
 */
static inline uint8_t Mt6701_Decode(Mt6701_Link_t *link, const uint8_t raw[3],
                                    uint16_t *angle_out, uint8_t *status_out)
{
  const uint32_t frame24 = ((uint32_t)raw[0] << 16U) | ((uint32_t)raw[1] << 8U) | (uint32_t)raw[2];
  uint16_t angle;

  if (frame24 == 0U)
  {
    if ((link->have_last == 0U) ||
        ((link->last_raw > MT6701_ZERO_WINDOW) &&
         (link->last_raw < (uint16_t)(MT6701_CPR - MT6701_ZERO_WINDOW))))
    {
      return 0U;
    }
  }
  else if (Mt6701_CalcCrc6((frame24 >> 6U) & 0x3FFFFU) != (uint8_t)(frame24 & 0x3FU))
  {
    return 0U;
  }

  angle = (uint16_t)((frame24 >> 10U) & 0x3FFFU);
  link->last_raw = angle;
  link->have_last = 1U;
  *angle_out = angle;
  if (status_out != 0)
  {
    *status_out = (uint8_t)((frame24 >> 6U) & 0x0FU);
  }
  return 1U;
}

#ifdef __cplusplus
}
#endif

#endif /* MT6701_H */
