/*
 * pet_vectors.h — 协议参考实现 + 测试向量（无需第三方依赖）
 *
 * 与 core/pet_protocol.h 的关系：
 *   - core/pet_protocol.h/.c 是**固件侧实现**
 *   - 本模块是**独立编写的参考实现**，用于交叉验证固件实现
 *   - PETV_CASES[] 是测试向量的唯一定义处
 */
#ifndef PET_VECTORS_H
#define PET_VECTORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet_protocol.h"   /* 交叉验证需要固件侧实现 */

#ifdef __cplusplus
extern "C" {
#endif

#define PETV_COMPANY_ID        0xFFFFu
#define PETV_PROTOCOL_VERSION  0x01u
#define PETV_PAYLOAD_LEN       14u
#define PETV_CRC_COVER_LEN     13u
#define PETV_CRC_FIELD_OFFSET  13u
#define PETV_MFG_LEN           (PETV_PAYLOAD_LEN + 2u)   /* 16 */

/** 一组用于构造向量的参数 */
typedef struct {
    uint32_t device_id;
    uint16_t sequence;
    uint8_t  activity_state;
    uint8_t  activity_score;
    uint8_t  battery_percent;
    uint8_t  flags;
} petv_params_t;

/** 一个测试向量：参数 + 期望的完整 16 字节 MFG */
typedef struct {
    const char     *name;
    petv_params_t   params;
    uint8_t         expect[PETV_MFG_LEN];
} petv_case_t;

extern const petv_case_t PETV_CASES[];
extern const size_t      PETV_CASE_COUNT;

/* --- 参考实现 --- */

uint8_t petv_crc8(const uint8_t *data, size_t len);
void    petv_build(const petv_params_t *p, uint8_t out[PETV_MFG_LEN]);
bool    petv_verify(const uint8_t mfg[PETV_MFG_LEN]);
bool    petv_seq_is_newer(uint16_t s, uint16_t last);

/* --- 输出 --- */

/** 把字节数组格式化成 "AA BB CC"；out 至少需要 3*n+1 字节 */
void petv_hex(const uint8_t *b, size_t n, char *out, size_t out_len);

/**
 * 完整自检：向量比对 + 负例 + 序号回绕 + 参考实现与固件实现交叉验证。
 * @return 失败项数（0 = 全通过）
 */
int petv_self_test(bool verbose);

/** 打印所有测试向量报告；内部会调用 petv_self_test(false) */
int petv_report_vectors(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_VECTORS_H */
