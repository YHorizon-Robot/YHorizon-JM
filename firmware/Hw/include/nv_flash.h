#ifndef NV_FLASH_H
#define NV_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  标定记录所在 Flash 页的起始地址。页位置由当前板卡决定。
 */
uint32_t NvFlash_PageAddr(void);

/**
 * @brief  从映射地址读出 Flash 内容。
 */
void NvFlash_Read(uint32_t addr, void *dst, uint32_t len);

/**
 * @brief  关中断、擦除整页、写入 len 字节，再清掉期间挂起的控制中断。
 * @param len 字节数，须为 4 的倍数且不超过一页。
 * @return 1 成功，0 失败。
 */
uint8_t NvFlash_WritePage(const void *data, uint32_t len);
/** Replace one of the two commissioning pages immediately before the legacy page. */
uint8_t NvFlash_WriteCommissionPage(uint8_t slot, const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* NV_FLASH_H */
