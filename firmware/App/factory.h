#ifndef FACTORY_H
#define FACTORY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  工厂测试。烧录后以 0.1 标幺母线电压开环强拖，
 *         经 RTT 打印两路 MT6701 角度以及时钟、母线、电流、CAN、磁铁故障。
 *         不返回。
 */
void Factory_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_H */
