#include "cali_nv.h"

#include <stddef.h>
#include <string.h>

#include "config.h"
#include "nv_flash.h"

#define CALI_NV_MAGIC      (0x59464F43UL) /* "YFOC" */
#define CALI_NV_VERSION_V1 (1U)
#define CALI_NV_VERSION_V2 (2U)
#define CALI_NV_VERSION_V3 (3U)
#define CALI_NV_VERSION    (4U)
#define CALI_NV_FLAG_GAINS (1U)
/**
 * @brief 记录带有游标第二编码器零偏，值在 reserved_v3 的 bit8…15（int8）。
 *
 * 沿用 V4 的长度和版本号：旧固件只认 GAINS 标志，也不读这 8 位，降级后照常加载。
 */
#define CALI_NV_FLAG_ENC2_OFFSET (2U)

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t pole_pairs;
  int16_t  encoder_dir;
  int16_t  closed_loop_dir;
  float    electrical_offset_rad;
  uint32_t reserved[3];
  uint32_t crc32;
} CaliNvRecordV2_t;

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t pole_pairs;
  int16_t  encoder_dir;
  int16_t  closed_loop_dir;
  float    electrical_offset_rad;
  uint32_t reserved[3];
  float    mt_zero_rad;
  uint32_t reserved_v3;
  uint32_t crc32;
} CaliNvRecord_t;

_Static_assert(sizeof(CaliNvRecordV2_t) == 32U, "CaliNvRecordV2_t must be 32 bytes");
_Static_assert(sizeof(CaliNvRecord_t) == 40U, "CaliNvRecord_t must be 40 bytes");

static uint32_t CaliNv_Crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t i;
  uint32_t j;

  for (i = 0U; i < len; i++)
  {
    crc ^= (uint32_t)data[i];
    for (j = 0U; j < 8U; j++)
    {
      const uint32_t mask = 0U - (crc & 1UL);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

static uint32_t CaliNv_RecordCrc(const CaliNvRecord_t *rec)
{
  return CaliNv_Crc32((const uint8_t *)rec, offsetof(CaliNvRecord_t, crc32));
}

static uint32_t CaliNv_RecordCrcV2(const CaliNvRecordV2_t *rec)
{
  return CaliNv_Crc32((const uint8_t *)rec, offsetof(CaliNvRecordV2_t, crc32));
}

static uint8_t CaliNv_HeaderOk(const CaliNvRecord_t *rec)
{
  if (rec->magic != CALI_NV_MAGIC)
  {
    return 0U;
  }
  if ((rec->version != CALI_NV_VERSION) && (rec->version != CALI_NV_VERSION_V3))
  {
    return 0U;
  }
  if (rec->crc32 != CaliNv_RecordCrc(rec))
  {
    return 0U;
  }
  return 1U;
}

static uint8_t CaliNv_HeaderOkV2(const CaliNvRecordV2_t *rec)
{
  if (rec->magic != CALI_NV_MAGIC)
  {
    return 0U;
  }
  if ((rec->version != CALI_NV_VERSION_V1) && (rec->version != CALI_NV_VERSION_V2))
  {
    return 0U;
  }
  if (rec->crc32 != CaliNv_RecordCrcV2(rec))
  {
    return 0U;
  }
  return 1U;
}

static uint8_t CaliNv_NodeIdOk(uint32_t id)
{
  const uint8_t nid = (uint8_t)(id & 0xFFU);
  return ((nid >= CFG_NODE_ID_MIN) && (nid <= CFG_NODE_ID_MAX)) ? 1U : 0U;
}

static void CaliNv_ClearGains(CaliNvData_t *out)
{
  out->user_gains_valid = 0U;
  out->vel_kp_raw = 0U;
  out->vel_ki_raw = 0U;
  out->pos_kp_raw = 0U;
  out->pos_ki_raw = 0U;
  out->pos_kd_raw = 0U;
}

static void CaliNv_UnpackCommon(uint16_t version,
                               uint16_t pole_pairs,
                               int16_t encoder_dir,
                               int16_t closed_loop_dir,
                               float electrical_offset_rad,
                               const uint32_t reserved[3],
                               CaliNvData_t *out)
{
  const uint32_t w0 = reserved[0];
  const uint32_t w1 = reserved[1];
  const uint32_t w2 = reserved[2];

  out->user_gains_inner_loop = CFG_INNER_LOOP_VOLTAGE;
  out->user_gains_scale_milli = 1000U;
  out->enc2_offset_valid = 0U;
  out->enc2_offset_raw = 0;

  out->pole_pairs = (uint8_t)pole_pairs;
  out->encoder_dir = (int8_t)encoder_dir;
  out->closed_loop_dir = (int8_t)closed_loop_dir;
  out->electrical_offset_rad = electrical_offset_rad;
  out->node_id = CaliNv_NodeIdOk(w0) ? (uint8_t)(w0 & 0xFFU) : 0U;
  if (version >= CALI_NV_VERSION_V2)
  {
    out->user_gains_valid = (((w0 >> 8) & 0xFFU) & CALI_NV_FLAG_GAINS) ? 1U : 0U;
    out->vel_kp_raw = (uint16_t)(w1 & 0xFFFFU);
    out->vel_ki_raw = (uint16_t)(w1 >> 16);
    out->pos_kp_raw = (uint16_t)(w2 & 0xFFFFU);
    out->pos_ki_raw = (uint16_t)(w2 >> 16);
    out->pos_kd_raw = (uint16_t)(w0 >> 16);
  }
  else
  {
    CaliNv_ClearGains(out);
  }
}

static void CaliNv_UnpackV2(const CaliNvRecordV2_t *rec, CaliNvData_t *out)
{
  CaliNv_UnpackCommon(rec->version, rec->pole_pairs, rec->encoder_dir,
                      rec->closed_loop_dir, rec->electrical_offset_rad,
                      rec->reserved, out);
  out->mt_zero_rad = CFG_MULTI_TURN_ZERO_RAD;
}

static void CaliNv_Unpack(const CaliNvRecord_t *rec, CaliNvData_t *out)
{
  CaliNv_UnpackCommon(rec->version, rec->pole_pairs, rec->encoder_dir,
                      rec->closed_loop_dir, rec->electrical_offset_rad,
                      rec->reserved, out);
  out->mt_zero_rad = rec->mt_zero_rad;
  if (rec->version >= CALI_NV_VERSION)
  {
    out->user_gains_inner_loop = (uint8_t)(rec->reserved_v3 & 0xFFU);
    out->user_gains_scale_milli = (uint16_t)(rec->reserved_v3 >> 16);
    if ((((rec->reserved[0] >> 8) & 0xFFU) & CALI_NV_FLAG_ENC2_OFFSET) != 0U)
    {
      out->enc2_offset_valid = 1U;
      out->enc2_offset_raw = (int8_t)(uint8_t)((rec->reserved_v3 >> 8) & 0xFFU);
    }
  }
}

static void CaliNv_Pack(CaliNvRecord_t *rec, const CaliNvData_t *in)
{
  uint8_t flags = 0U;
  uint8_t enc2_offset = 0U;
  uint16_t pos_kd = 0U;
  uint16_t vel_kp = 0U;
  uint16_t vel_ki = 0U;
  uint16_t pos_kp = 0U;
  uint16_t pos_ki = 0U;

  memset(rec, 0, sizeof(*rec));
  rec->magic = CALI_NV_MAGIC;
  rec->version = CALI_NV_VERSION;
  rec->pole_pairs = (uint16_t)in->pole_pairs;
  rec->encoder_dir = (int16_t)((in->encoder_dir < 0) ? -1 : 1);
  rec->closed_loop_dir = (int16_t)((in->closed_loop_dir < 0) ? -1 : 1);
  rec->electrical_offset_rad = in->electrical_offset_rad;
  if (in->user_gains_valid != 0U)
  {
    flags = CALI_NV_FLAG_GAINS;
    vel_kp = in->vel_kp_raw;
    vel_ki = in->vel_ki_raw;
    pos_kp = in->pos_kp_raw;
    pos_ki = in->pos_ki_raw;
    pos_kd = in->pos_kd_raw;
  }
  if (in->enc2_offset_valid != 0U)
  {
    flags |= CALI_NV_FLAG_ENC2_OFFSET;
    enc2_offset = (uint8_t)in->enc2_offset_raw;
  }
  rec->reserved[0] = (uint32_t)(CaliNv_NodeIdOk(in->node_id) ? in->node_id : 0U) |
                     ((uint32_t)flags << 8) |
                     ((uint32_t)pos_kd << 16);
  rec->reserved[1] = (uint32_t)vel_kp | ((uint32_t)vel_ki << 16);
  rec->reserved[2] = (uint32_t)pos_kp | ((uint32_t)pos_ki << 16);
  rec->mt_zero_rad = in->mt_zero_rad;
  rec->reserved_v3 = (uint32_t)in->user_gains_inner_loop |
                     ((uint32_t)enc2_offset << 8) |
                     ((uint32_t)in->user_gains_scale_milli << 16);
  rec->crc32 = CaliNv_RecordCrc(rec);
}

static uint8_t CaliNv_ReadData(CaliNvData_t *out)
{
  uint16_t ver;

  if (out == 0)
  {
    return 0U;
  }

  NvFlash_Read(NvFlash_PageAddr() + 4UL, &ver, sizeof(ver));

  if ((ver == CALI_NV_VERSION_V1) || (ver == CALI_NV_VERSION_V2))
  {
    CaliNvRecordV2_t rec;
    NvFlash_Read(NvFlash_PageAddr(), &rec, sizeof(rec));
    if (CaliNv_HeaderOkV2(&rec) == 0U)
    {
      return 0U;
    }
    CaliNv_UnpackV2(&rec, out);
    return 1U;
  }
  if ((ver == CALI_NV_VERSION) || (ver == CALI_NV_VERSION_V3))
  {
    CaliNvRecord_t rec;
    NvFlash_Read(NvFlash_PageAddr(), &rec, sizeof(rec));
    if (CaliNv_HeaderOk(&rec) == 0U)
    {
      return 0U;
    }
    CaliNv_Unpack(&rec, out);
    return 1U;
  }
  return 0U;
}

static uint8_t CaliNv_WriteData(const CaliNvData_t *in)
{
  CaliNvRecord_t rec;

  CaliNv_Pack(&rec, in);
  return NvFlash_WritePage(&rec, sizeof(rec));
}

static uint8_t CaliNv_DataValid(const CaliNvData_t *data)
{
  if ((data->pole_pairs < CFG_CALI_PP_MIN) || (data->pole_pairs > CFG_CALI_PP_MAX))
  {
    return 0U;
  }
  if ((data->encoder_dir != 1) && (data->encoder_dir != -1))
  {
    return 0U;
  }
  if ((data->closed_loop_dir != 1) && (data->closed_loop_dir != -1))
  {
    return 0U;
  }
  return 1U;
}

uint8_t CaliNv_Load(CaliNvData_t *out)
{
  if ((out == 0) || (CaliNv_ReadData(out) == 0U))
  {
    return 0U;
  }
  return CaliNv_DataValid(out);
}

uint8_t CaliNv_LoadNodeId(void)
{
  CaliNvData_t data;

  if (CaliNv_ReadData(&data) == 0U)
  {
    return 0U;
  }
  return data.node_id;
}

uint8_t CaliNv_LoadMetadata(CaliNvData_t *out)
{
  return (out != 0) ? CaliNv_ReadData(out) : 0U;
}

uint8_t CaliNv_Save(const CaliNvData_t *in)
{
  CaliNvData_t rec;
  uint8_t ok;

  if (in == 0)
  {
    return 0U;
  }
  if ((in->pole_pairs < CFG_CALI_PP_MIN) || (in->pole_pairs > CFG_CALI_PP_MAX))
  {
    return 0U;
  }

  if (CaliNv_ReadData(&rec) == 0U)
  {
    memset(&rec, 0, sizeof(rec));
  }
  rec.pole_pairs = in->pole_pairs;
  rec.encoder_dir = (in->encoder_dir < 0) ? -1 : 1;
  rec.closed_loop_dir = (in->closed_loop_dir < 0) ? -1 : 1;
  rec.electrical_offset_rad = in->electrical_offset_rad;
  rec.mt_zero_rad = in->mt_zero_rad;
  rec.node_id = CaliNv_NodeIdOk(in->node_id) ? in->node_id : rec.node_id;
  rec.enc2_offset_valid = (in->enc2_offset_valid != 0U) ? 1U : 0U;
  rec.enc2_offset_raw = (in->enc2_offset_valid != 0U) ? in->enc2_offset_raw : 0;

  ok = CaliNv_WriteData(&rec);
  if (ok != 0U)
  {
    CaliNvData_t check;
    if ((CaliNv_Load(&check) == 0U) ||
        (check.pole_pairs != rec.pole_pairs) ||
        (check.encoder_dir != rec.encoder_dir) ||
        (check.closed_loop_dir != rec.closed_loop_dir) ||
        (check.enc2_offset_valid != rec.enc2_offset_valid) ||
        (check.enc2_offset_raw != rec.enc2_offset_raw))
    {
      ok = 0U;
    }
  }
  return ok;
}

uint8_t CaliNv_SaveNodeId(uint8_t node_id)
{
  CaliNvData_t rec;

  if (CaliNv_NodeIdOk(node_id) == 0U)
  {
    return 0U;
  }

  if (CaliNv_ReadData(&rec) == 0U)
  {
    memset(&rec, 0, sizeof(rec));
  }
  rec.node_id = node_id;
  if (CaliNv_WriteData(&rec) == 0U)
  {
    return 0U;
  }
  return (CaliNv_LoadNodeId() == node_id) ? 1U : 0U;
}

uint8_t CaliNv_SaveUserGains(uint8_t node_id,
                             uint8_t inner_loop,
                             uint16_t vel_kp_raw,
                             uint16_t vel_ki_raw,
                             uint16_t pos_kp_raw,
                             uint16_t pos_ki_raw,
                             uint16_t pos_kd_raw)
{
  CaliNvData_t rec;

  if (inner_loop > CFG_INNER_LOOP_CURRENT) { return 0U; }
  if (CaliNv_ReadData(&rec) == 0U)
  {
    memset(&rec, 0, sizeof(rec));
    rec.node_id = CaliNv_NodeIdOk(node_id) ? node_id : 0U;
  }
  rec.user_gains_valid = 1U;
  rec.user_gains_inner_loop = inner_loop;
  rec.user_gains_scale_milli = (inner_loop == CFG_INNER_LOOP_CURRENT)
      ? (uint16_t)((CFG_I_LIMIT_A / CFG_V_LIMIT) * 1000.0f + 0.5f) : 1000U;
  rec.vel_kp_raw = vel_kp_raw;
  rec.vel_ki_raw = vel_ki_raw;
  rec.pos_kp_raw = pos_kp_raw;
  rec.pos_ki_raw = pos_ki_raw;
  rec.pos_kd_raw = pos_kd_raw;
  if (CaliNv_WriteData(&rec) == 0U)
  {
    return 0U;
  }
  if (CaliNv_ReadData(&rec) == 0U)
  {
    return 0U;
  }
  return ((rec.user_gains_valid != 0U) &&
          (rec.user_gains_inner_loop == inner_loop) &&
          (rec.vel_kp_raw == vel_kp_raw) &&
          (rec.vel_ki_raw == vel_ki_raw) &&
          (rec.pos_kp_raw == pos_kp_raw) &&
          (rec.pos_ki_raw == pos_ki_raw) &&
          (rec.pos_kd_raw == pos_kd_raw)) ? 1U : 0U;
}

uint8_t CaliNv_SaveMtZero(float mt_zero_rad)
{
  CaliNvData_t rec;

  if (CaliNv_ReadData(&rec) == 0U)
  {
    return 0U;
  }
  rec.mt_zero_rad = mt_zero_rad;
  if (CaliNv_WriteData(&rec) == 0U)
  {
    return 0U;
  }
  if (CaliNv_ReadData(&rec) == 0U)
  {
    return 0U;
  }
  return (rec.mt_zero_rad == mt_zero_rad) ? 1U : 0U;
}
