/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T970Apr.h"

typedef struct {uint8_t *p; uint32_t n;} W;

static void put8(W *w, uint32_t v) {if (w->n < APR_MAX_PACKET) w->p[w->n] = (uint8_t)v; w->n++;}
static void put16(W *w, uint32_t v) {put8(w, v); put8(w, v >> 8);}
static void put32(W *w, uint32_t v) {put16(w, v); put16(w, v >> 16);}
static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

/* hdr_field: message type SEQ_CMD (2), header length 5 words, version 0; the packet size is patched in by finish(). */
static void begin(W *w, uint8_t *out, uint32_t svc, uint16_t port, uint32_t token, uint32_t opcode)
{
    w->p = out; w->n = 0;
    put16(w, 2u << 8 | (APR_HDR_BYTES / 4) << 4);
    put16(w, 0);
    put8(w, svc); put8(w, APR_DOMAIN_APPS); put16(w, port);
    put8(w, svc); put8(w, APR_DOMAIN_ADSP); put16(w, port);
    put32(w, token); put32(w, opcode);
}
static uint32_t finish(W *w)
{
    if (w->n > APR_MAX_PACKET) return 0;
    w->p[2] = (uint8_t)w->n; w->p[3] = (uint8_t)(w->n >> 8);
    return w->n;
}

uint32_t apr_core(uint8_t *out, uint32_t token, uint32_t opcode)
{
    W w;
    begin(&w, out, APR_SVC_CORE, 0, token, opcode);
    return finish(&w);
}

uint32_t apr_afe_hw_vote(uint8_t *out, uint32_t token, uint32_t block, const char *client)
{
    W w;
    begin(&w, out, APR_SVC_AFE, 0, token, AFE_CMD_HW_VOTE);
    put32(&w, block);
    unsigned i = 0;
    for (; i < 7 && client[i]; ++i) put8(&w, (uint8_t)client[i]);   /* char[8], NUL terminated (strscpy) */
    for (; i < 8; ++i) put8(&w, 0);
    return finish(&w);
}

/* AFE_SVC_CMD_SET_PARAM (port 0) or AFE_PORT_CMD_SET_PARAM_V2 (port != 0) with one in-band parameter of size bytes. */
static void param_begin(W *w, uint8_t *out, uint32_t token, uint16_t port, uint32_t module, uint32_t param, uint32_t size)
{
    begin(w, out, APR_SVC_AFE, 0, token, port ? AFE_PORT_CMD_SET_PARAM_V2 : AFE_SVC_CMD_SET_PARAM);
    if (port) {put16(w, port); put16(w, 12 + size);}
    else put32(w, 12 + size);
    put32(w, 0); put32(w, 0); put32(w, 0);              /* in band: no address, no map handle */
    put32(w, module); put32(w, param); put16(w, size); put16(w, 0);
}

uint32_t apr_afe_clock(uint8_t *out, uint32_t token, uint16_t port, uint32_t clock_id, uint32_t hz, uint16_t attribute)
{
    W w;
    param_begin(&w, out, token, port, AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, 20);
    put32(&w, 1); put32(&w, clock_id); put32(&w, hz); put16(&w, attribute); put16(&w, 0); put32(&w, hz != 0);
    return finish(&w);
}

uint32_t apr_afe_group_tdm(uint8_t *out, uint32_t token, const AprTdm *t)
{
    W w;
    param_begin(&w, out, token, 0, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_TDM_CONFIG, 44);
    put32(&w, 1); put16(&w, t->group); put16(&w, 0);
    put16(&w, t->port);
    for (unsigned i = 1; i < 8; ++i) put16(&w, 0xffff);
    /* As msm-dai-q6-v2.c: the group carries every slot at the slot width. */
    put32(&w, t->slots); put32(&w, t->rate); put32(&w, t->slot_bits);
    put16(&w, t->slots); put16(&w, t->slot_bits); put32(&w, t->slot_mask);
    return finish(&w);
}

uint32_t apr_afe_group_enable(uint8_t *out, uint32_t token, uint16_t group, int enable)
{
    W w;
    param_begin(&w, out, token, 0, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_ENABLE, 4);
    put16(&w, group); put16(&w, enable ? 1 : 0);
    return finish(&w);
}

uint32_t apr_afe_tdm_config(uint8_t *out, uint32_t token, const AprTdm *t)
{
    W w;
    param_begin(&w, out, token, t->port, AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, 36);
    put32(&w, 1); put32(&w, t->channels); put32(&w, t->rate); put32(&w, t->bits);
    put16(&w, 0);                                            /* linear PCM */
    put16(&w, t->sync_mode); put16(&w, t->sync_src); put16(&w, t->slots);
    put16(&w, t->data_out); put16(&w, t->invert_sync); put16(&w, t->data_delay); put16(&w, t->slot_bits);
    put32(&w, t->slot_mask);
    return finish(&w);
}

/* Channel n in the frame at byte offset n * slot bytes, data MSB aligned; unused entries 0xffff. */
uint32_t apr_afe_slot_mapping(uint8_t *out, uint32_t token, const AprTdm *t, unsigned version)
{
    W w;
    const unsigned offsets = version == 2 ? 32 : 8;
    param_begin(&w, out, token, t->port, AFE_MODULE_TDM, AFE_PARAM_ID_SLOT_MAPPING, 12 + 2 * offsets);
    put32(&w, version == 2 ? 2 : 1); put16(&w, t->channels); put16(&w, t->bits); put32(&w, 0);
    for (unsigned i = 0; i < offsets; ++i) put16(&w, i < t->channels ? i * (t->slot_bits / 8u) : 0xffffu);
    return finish(&w);
}

uint32_t apr_afe_port(uint8_t *out, uint32_t token, uint32_t opcode, uint16_t port)
{
    W w;
    begin(&w, out, APR_SVC_AFE, 0, token, opcode);
    put16(&w, port); put16(&w, 0);
    return finish(&w);
}

/* PCM channel map for 1, 2 or 4 channels (q6dsp_map_channels): FC; FL FR; FL FR LS RS. */
static void channel_map(W *w, uint16_t channels)
{
    static const uint8_t four[4] = {1, 2, 4, 5};
    for (unsigned i = 0; i < 8; ++i) put8(w, channels == 1 ? (i == 0 ? 3 : 0) : i < channels ? four[i] : 0);
}

uint32_t apr_adm_open(uint8_t *out, uint32_t token, uint16_t port, uint32_t rate, uint16_t channels, uint16_t bits)
{
    return apr_adm_open_path(out, token, port, 1, rate, channels, bits);
}

uint32_t apr_adm_open_path(uint8_t *out, uint32_t token, uint16_t port, uint16_t path, uint32_t rate, uint16_t channels, uint16_t bits)
{
    W w;
    begin(&w, out, APR_SVC_ADM, port, token, ADM_CMD_DEVICE_OPEN_V5);
    put16(&w, 0);                 /* legacy device session */
    put16(&w, path);              /* 1 playback (RX), 2 capture (TX) */
    put16(&w, port); put16(&w, 0xffff);
    put32(&w, ADM_NULL_COPP_TOPOLOGY);
    put16(&w, channels); put16(&w, bits); put32(&w, rate);
    channel_map(&w, channels);
    return finish(&w);
}

uint32_t apr_adm_close(uint8_t *out, uint32_t token, uint16_t port, uint16_t copp)
{
    W w;
    begin(&w, out, APR_SVC_ADM, port, token, ADM_CMD_DEVICE_CLOSE_V5);
    out[10] = (uint8_t)copp; out[11] = (uint8_t)(copp >> 8);  /* the COPP is the destination port */
    return finish(&w);
}

uint32_t apr_adm_map(uint8_t *out, uint32_t token, uint16_t session, uint16_t copp)
{
    return apr_adm_map_matrix(out, token, ADM_MATRIX_ID_AUDIO_RX, session, copp);
}

uint32_t apr_adm_map_matrix(uint8_t *out, uint32_t token, uint32_t matrix, uint16_t session, uint16_t copp)
{
    W w;
    begin(&w, out, APR_SVC_ADM, 0, token, ADM_CMD_MATRIX_MAP_V5);
    put32(&w, matrix);
    put32(&w, 1);                 /* one session */
    put16(&w, session); put16(&w, 1);
    put16(&w, copp); put16(&w, 0);
    return finish(&w);
}

static uint16_t stream_port(uint16_t session) {return (uint16_t)((session << 8 & 0xff00) | 1);}

uint32_t apr_asm_open_write(uint8_t *out, uint16_t session, uint16_t bits)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), session, ASM_STREAM_CMD_OPEN_WRITE_V3);
    put32(&w, 0);                 /* legacy stream session */
    put16(&w, 0);                 /* sink: device matrix */
    put16(&w, bits);
    put32(&w, ASM_NULL_POPP_TOPOLOGY);
    put32(&w, ASM_FMT_MULTI_CHANNEL_PCM_V2);
    return finish(&w);
}

uint32_t apr_asm_media_format(uint8_t *out, uint16_t session, uint32_t rate, uint16_t channels, uint16_t bits)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), session, ASM_DATA_CMD_MEDIA_FMT_V2);
    put32(&w, 20);                /* the format block after this word */
    put16(&w, channels); put16(&w, bits); put32(&w, rate); put16(&w, 1); put16(&w, 0);
    channel_map(&w, channels);
    return finish(&w);
}

uint32_t apr_asm_session(uint8_t *out, uint16_t session, uint32_t opcode)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), session, opcode);
    if (opcode == ASM_SESSION_CMD_RUN_V2) {put32(&w, 0); put32(&w, 0); put32(&w, 0);}   /* start now */
    return finish(&w);
}

uint32_t apr_asm_map(uint8_t *out, uint16_t session, uint64_t address, uint32_t bytes)
{
    W w;
    begin(&w, out, APR_SVC_ASM, 0, (uint32_t)session << 8, ASM_CMD_MEM_MAP);
    put16(&w, 3);                 /* ADSP_MEMORY_MAP_SHMEM8_4K_POOL */
    put16(&w, 1); put32(&w, 0);   /* one region, physical */
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, (bytes + 4095u) & ~4095u);
    return finish(&w);
}

uint32_t apr_asm_unmap(uint8_t *out, uint16_t session, uint32_t handle)
{
    W w;
    begin(&w, out, APR_SVC_ASM, 0, (uint32_t)session << 8, ASM_CMD_MEM_UNMAP);
    put32(&w, handle);
    return finish(&w);
}

uint32_t apr_asm_write(uint8_t *out, uint16_t session, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes, uint32_t seq)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), token, ASM_DATA_CMD_WRITE_V2);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes); put32(&w, seq);
    put32(&w, 0); put32(&w, 0); put32(&w, 0);   /* no time stamp, no flags */
    return finish(&w);
}

/* AFE_PARAM_ID_CODEC_DMA_CONFIG: minor 1, rate, bit width, data format 0 (linear PCM), channels, active channel (decimator) mask. */
uint32_t apr_afe_cdc_dma(uint8_t *out, uint32_t token, uint16_t port, uint32_t rate, uint16_t bits, uint16_t channels, uint16_t mask)
{
    W w;
    param_begin(&w, out, token, port, AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_CODEC_DMA_CONFIG, 16);
    put32(&w, 1); put32(&w, rate); put16(&w, bits); put16(&w, 0); put16(&w, channels); put16(&w, mask);
    return finish(&w);
}

uint32_t apr_asm_open_read(uint8_t *out, uint16_t session, uint16_t bits)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), session, ASM_STREAM_CMD_OPEN_READ_V3);
    put32(&w, 0);                 /* legacy stream session, no time stamps */
    put32(&w, 0);                 /* source: device matrix */
    put32(&w, ASM_NULL_POPP_TOPOLOGY);
    put32(&w, ASM_FMT_MULTI_CHANNEL_PCM_V2);
    put16(&w, bits); put16(&w, 0);
    return finish(&w);
}

/* ASM_STREAM_CMD_SET_ENCDEC_PARAM / ENC_CFG_BLK_V2 with the PCM block (q6asm_enc_cfg_blk_pcm_v2): param size 28 (frames per buffer,
 * block size 20, then the 20-byte PCM block), frames per buffer 0. */
uint32_t apr_asm_enc_pcm(uint8_t *out, uint16_t session, uint32_t rate, uint16_t channels, uint16_t bits)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), session, ASM_STREAM_CMD_SET_ENCDEC_PARAM);
    put32(&w, ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2); put32(&w, 28);
    put32(&w, 0); put32(&w, 20);
    put16(&w, channels); put16(&w, bits); put32(&w, rate); put16(&w, 1); put16(&w, 0);
    channel_map(&w, channels);
    return finish(&w);
}

uint32_t apr_asm_read(uint8_t *out, uint16_t session, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes, uint32_t seq)
{
    W w;
    begin(&w, out, APR_SVC_ASM, stream_port(session), token, ASM_DATA_CMD_READ_V2);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes); put32(&w, seq);
    return finish(&w);
}

int apr_parse(const uint8_t *in, uint32_t length, AprAnswer *a)
{
    for (unsigned i = 0; i < sizeof *a; ++i) ((uint8_t *)a)[i] = 0;
    if (length < APR_HDR_BYTES) return AprBad;
    const uint16_t field = le16(in), size = le16(in + 2);
    const uint32_t hdr = ((field >> 4) & 15u) * 4u;
    if (hdr < APR_HDR_BYTES || size < hdr || size > length) return AprBad;
    a->src_svc = in[4]; a->src_domain = in[5]; a->src_port = le16(in + 6); a->dest_port = le16(in + 10);
    a->token = le32(in + 12); a->opcode = le32(in + 16);
    const uint8_t *d = in + hdr;
    const uint32_t n = a->payload_bytes = size - hdr;
    switch (a->opcode) {
    case APR_BASIC_RSP_RESULT:
        if (n < 8) return AprBad;
        a->result_opcode = le32(d); a->status = le32(d + 4);
        return AprBasic;
    case AVCS_GET_STATE_RSP:
        if (n < 4) return AprBad;
        a->status = le32(d);
        return AprCoreState;
    case AVCS_GET_FWK_VERSION_RSP:
        if (n < 20) return AprBad;
        for (unsigned i = 0; i < 5; ++i) a->words[i] = le32(d + 4 * i);
        return AprCoreFwk;
    case AVCS_GET_VERSIONS_RSP:
        if (n < 8) return AprBad;
        a->words[0] = le32(d); a->words[1] = le32(d + 4);
        return AprCoreVersions;
    case ADM_CMDRSP_DEVICE_OPEN_V5:
        if (n < 6) return AprBad;
        a->status = le32(d); a->value = le16(d + 4);
        return AprAdmOpened;
    case ASM_CMDRSP_MEM_MAP:
        if (n < 4) return AprBad;
        a->value = le32(d);
        return AprMapped;
    case ASM_DATA_EVENT_WRITE_DONE_V2:
        if (n < 16) return AprBad;
        a->address = le32(d) | (uint64_t)le32(d + 4) << 32; a->value = le32(d + 8); a->status = le32(d + 12);
        return AprWriteDone;
    case ASM_DATA_EVENT_READ_DONE_V2:   /* status, address lo/hi, handle, size, offset, time stamp lo/hi, flags, frames */
        if (n < 36) return AprBad;
        a->status = le32(d); a->address = le32(d + 4) | (uint64_t)le32(d + 8) << 32; a->value = le32(d + 12);
        for (unsigned i = 0; i < 5; ++i) a->words[i] = le32(d + 16 + 4 * i);
        return AprReadDone;
    case AFE_CMD_HW_VOTE_RSP:
        if (n < 4) return AprBad;
        a->status = le32(d);
        return AprVote;
    default:
        return AprOther;
    }
}
