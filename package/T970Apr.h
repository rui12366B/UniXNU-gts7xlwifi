/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* APR packets for the gts7xlwifi audio DSP (Elite firmware on the "apr_audio_svc" GLINK channel): the 20-byte APR header and the
 * commands the kernel test tone needs from the core (AVCS), AFE, ADM and ASM services. Layouts and opcodes follow Linux
 * drivers/soc/qcom/apr.c, include/linux/soc/qcom/apr.h and sound/soc/qcom/qdsp6/q6{core,afe,adm,asm}.c (v7.0.14); the TDM group
 * device parameters follow the downstream q6afe interface the board's Android audio stack uses. Pure encoding and decoding: no I/O. */
#ifndef T970_APR_H
#define T970_APR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define APR_HDR_BYTES 20u
#define APR_MAX_PACKET 512u

#define APR_DOMAIN_ADSP 4u
#define APR_DOMAIN_APPS 5u
#define APR_SVC_CORE 3u
#define APR_SVC_AFE 4u
#define APR_SVC_ASM 7u
#define APR_SVC_ADM 8u

#define APR_BASIC_RSP_RESULT 0x000110E8u
#define APR_RSP_ACCEPTED 0x000100BEu
#define ADSP_EUNSUPPORTED 0x7u

/* core */
#define AVCS_GET_STATE 0x0001290Cu
#define AVCS_GET_STATE_RSP 0x0001290Du
#define AVCS_GET_VERSIONS 0x00012905u
#define AVCS_GET_VERSIONS_RSP 0x00012906u
#define AVCS_GET_FWK_VERSION 0x0001292Cu
#define AVCS_GET_FWK_VERSION_RSP 0x0001292Du

/* AFE */
#define AFE_PORT_CMD_DEVICE_START 0x000100E5u
#define AFE_PORT_CMD_DEVICE_STOP 0x000100E6u
#define AFE_PORT_CMD_SET_PARAM_V2 0x000100EFu
#define AFE_SVC_CMD_SET_PARAM 0x000100F3u
#define AFE_CMD_HW_VOTE 0x000100F4u
#define AFE_CMD_HW_VOTE_RSP 0x000100F5u
#define AFE_MODULE_AUDIO_DEV_INTERFACE 0x0001020Cu
#define AFE_MODULE_TDM 0x0001028Au
#define AFE_MODULE_CLOCK_SET 0x0001028Fu
#define AFE_MODULE_GROUP_DEVICE 0x00010254u
#define AFE_PARAM_ID_TDM_CONFIG 0x0001029Du
#define AFE_PARAM_ID_SLOT_MAPPING 0x00010297u
#define AFE_PARAM_ID_CLOCK_SET 0x00010290u
#define AFE_PARAM_ID_GROUP_ENABLE 0x00010256u
#define AFE_PARAM_ID_GROUP_TDM_CONFIG 0x0001029Eu
#define AFE_HW_BLOCK_MACRO 0x3u
#define AFE_HW_BLOCK_DCODEC 0x4u

/* ADM */
#define ADM_CMD_DEVICE_OPEN_V5 0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5 0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5 0x00010327u
#define ADM_CMD_MATRIX_MAP_V5 0x00010325u
#define ADM_NULL_COPP_TOPOLOGY 0x00010312u

/* ASM */
#define ASM_STREAM_CMD_OPEN_WRITE_V3 0x00010DB3u
#define ASM_STREAM_CMD_CLOSE 0x00010BCDu
#define ASM_SESSION_CMD_RUN_V2 0x00010DAAu
#define ASM_SESSION_CMD_PAUSE 0x00010BD3u
#define ASM_DATA_CMD_MEDIA_FMT_V2 0x00010D98u
#define ASM_DATA_CMD_WRITE_V2 0x00010DABu
#define ASM_DATA_EVENT_WRITE_DONE_V2 0x00010D99u
#define ASM_CMD_MEM_MAP 0x00010D92u
#define ASM_CMDRSP_MEM_MAP 0x00010D93u
#define ASM_CMD_MEM_UNMAP 0x00010D94u
#define ASM_NULL_POPP_TOPOLOGY 0x00010C68u
#define ASM_FMT_MULTI_CHANNEL_PCM_V2 0x00010DA5u

/* Capture (the microphones): codec DMA TX port, ADM TX path, ASM read stream (downstream apr_audio-v2.h, q6asm.c, q6afe.c). */
#define AFE_PARAM_ID_CODEC_DMA_CONFIG 0x000102B8u
#define AFE_PORT_ID_TX_CODEC_DMA_TX_3 0xB037u
#define AFE_PORT_ID_VA_CODEC_DMA_TX_0 0xB021u
#define ADM_MATRIX_ID_AUDIO_RX 0u
#define ADM_MATRIX_ID_AUDIO_TX 1u
#define ASM_STREAM_CMD_OPEN_READ_V3 0x00010DB4u
#define ASM_STREAM_CMD_SET_ENCDEC_PARAM 0x00010C10u
#define ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2 0x00010DA3u
#define ASM_DATA_CMD_READ_V2 0x00010DACu
#define ASM_DATA_EVENT_READ_DONE_V2 0x00010D9Au

/* The one TDM interface the speaker amplifiers sit on (live FDT /soc/qcom,msm-dai-tdm-pri-rx). */
typedef struct {
    uint16_t port, group;           /* 0x9000, 0x9100 */
    uint32_t rate, channels, bits;  /* 48000, 4, 16 */
    uint16_t slots, slot_bits;      /* 4, 32 */
    uint16_t sync_mode, sync_src, data_out, invert_sync, data_delay;
    uint32_t slot_mask;
} AprTdm;

/* Every builder writes one packet into out (APR_MAX_PACKET bytes) and returns its size. port: the APR source/destination port. */
uint32_t apr_core(uint8_t *out, uint32_t token, uint32_t opcode);
uint32_t apr_afe_hw_vote(uint8_t *out, uint32_t token, uint32_t block, const char *client);
uint32_t apr_afe_clock(uint8_t *out, uint32_t token, uint16_t port, uint32_t clock_id, uint32_t hz, uint16_t attribute);
uint32_t apr_afe_group_tdm(uint8_t *out, uint32_t token, const AprTdm *t);
uint32_t apr_afe_group_enable(uint8_t *out, uint32_t token, uint16_t group, int enable);
uint32_t apr_afe_tdm_config(uint8_t *out, uint32_t token, const AprTdm *t);
/* version 1: 8 offsets; version 2 (AFE API >= V3, what downstream sends on this firmware): 32 offsets. */
uint32_t apr_afe_slot_mapping(uint8_t *out, uint32_t token, const AprTdm *t, unsigned version);
uint32_t apr_afe_port(uint8_t *out, uint32_t token, uint32_t opcode, uint16_t port);
uint32_t apr_adm_open(uint8_t *out, uint32_t token, uint16_t port, uint32_t rate, uint16_t channels, uint16_t bits);
uint32_t apr_adm_close(uint8_t *out, uint32_t token, uint16_t port, uint16_t copp);
uint32_t apr_adm_map(uint8_t *out, uint32_t token, uint16_t session, uint16_t copp);
uint32_t apr_asm_open_write(uint8_t *out, uint16_t session, uint16_t bits);
uint32_t apr_asm_media_format(uint8_t *out, uint16_t session, uint32_t rate, uint16_t channels, uint16_t bits);
uint32_t apr_asm_session(uint8_t *out, uint16_t session, uint32_t opcode);   /* RUN_V2 (immediate), PAUSE, CLOSE */
uint32_t apr_asm_map(uint8_t *out, uint16_t session, uint64_t address, uint32_t bytes);
uint32_t apr_asm_unmap(uint8_t *out, uint16_t session, uint32_t handle);
uint32_t apr_asm_write(uint8_t *out, uint16_t session, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes, uint32_t seq);
/* Capture. path: 1 RX (playback), 2 TX (capture); matrix: ADM_MATRIX_ID_AUDIO_RX / _TX. */
uint32_t apr_afe_cdc_dma(uint8_t *out, uint32_t token, uint16_t port, uint32_t rate, uint16_t bits, uint16_t channels, uint16_t mask);
uint32_t apr_adm_open_path(uint8_t *out, uint32_t token, uint16_t port, uint16_t path, uint32_t rate, uint16_t channels, uint16_t bits);
uint32_t apr_adm_map_matrix(uint8_t *out, uint32_t token, uint32_t matrix, uint16_t session, uint16_t copp);
uint32_t apr_asm_open_read(uint8_t *out, uint16_t session, uint16_t bits);
uint32_t apr_asm_enc_pcm(uint8_t *out, uint16_t session, uint32_t rate, uint16_t channels, uint16_t bits);
uint32_t apr_asm_read(uint8_t *out, uint16_t session, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes, uint32_t seq);

/* An answer. kind: AprBasic (result_opcode, status), AprCoreState (status = state), AprCoreFwk (words[0..4]), AprCoreVersions
 * (words[0..1]), AprAdmOpened (status, value = copp), AprMapped (value = handle), AprWriteDone (address, value = handle, status),
 * AprVote (status = client handle), AprReadDone (status, address, value = handle, words[0] = bytes, words[1] = offset, words[2..3]
 * time stamp, words[4] = flags; src_port = the session's stream port), AprOther. */
typedef struct {
    uint32_t opcode, token, result_opcode, status, value;
    uint64_t address;
    uint32_t words[5];
    uint8_t src_svc, src_domain;
    uint16_t src_port, dest_port;
    uint32_t payload_bytes;
} AprAnswer;
enum {AprBad = -1, AprOther = 0, AprBasic, AprCoreState, AprCoreFwk, AprCoreVersions, AprAdmOpened, AprMapped, AprWriteDone, AprVote,
      AprReadDone};
int apr_parse(const uint8_t *in, uint32_t length, AprAnswer *a);

#ifdef __cplusplus
}
#endif
#endif
