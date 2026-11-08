/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* QRTR, QMI and SSC protobuf codecs for the sensor hub client (see T970Ssc.h). Integer only: the kernel builds this with
 * -mgeneral-regs-only. */
#include "T970Ssc.h"

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint16_t get16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static uint64_t get64(const uint8_t *p) {return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;}
static void copy(uint8_t *to, const uint8_t *from, size_t n) {for (size_t i = 0; i < n; ++i) to[i] = from[i];}

/* ---- QRTR ---- */
size_t t970_qrtr_pack(uint8_t *out, size_t cap, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node,
                      uint32_t dst_port, uint32_t confirm_rx, const void *payload, uint32_t size) {
    const size_t padded = (size + 3u) & ~(size_t)3;
    if (!out || cap < T970_QRTR_HEADER + padded) return 0;
    put32(out, 1); put32(out + 4, type); put32(out + 8, src_node); put32(out + 12, src_port);
    put32(out + 16, confirm_rx); put32(out + 20, size); put32(out + 24, dst_node); put32(out + 28, dst_port);
    if (size) copy(out + T970_QRTR_HEADER, (const uint8_t *)payload, size);
    for (size_t i = size; i < padded; ++i) out[T970_QRTR_HEADER + i] = 0;
    return T970_QRTR_HEADER + padded;
}
size_t t970_qrtr_ctrl(uint8_t *out, size_t cap, uint32_t cmd, uint32_t src_node, uint32_t dst_node, uint32_t a, uint32_t b,
                      uint32_t c, uint32_t d) {
    uint8_t body[20];
    put32(body, cmd); put32(body + 4, a); put32(body + 8, b); put32(body + 12, c); put32(body + 16, d);
    return t970_qrtr_pack(out, cap, cmd, src_node, T970_QRTR_PORT_CTRL, dst_node, T970_QRTR_PORT_CTRL, 0, body, sizeof body);
}
int t970_qrtr_parse(const uint8_t *data, size_t length, T970QrtrPacket *p) {
    if (!data || !p || length < T970_QRTR_HEADER || get32(data) != 1) return -1;
    p->type = get32(data + 4); p->src_node = get32(data + 8); p->src_port = get32(data + 12);
    p->confirm_rx = get32(data + 16); p->size = get32(data + 20); p->dst_node = get32(data + 24); p->dst_port = get32(data + 28);
    if (p->size > length - T970_QRTR_HEADER) return -2;
    p->payload = data + T970_QRTR_HEADER;
    return 0;
}
int t970_qrtr_parse_ctrl(const T970QrtrPacket *p, T970QrtrCtrl *c) {
    if (!p || !c || p->size < 4) return -1;
    c->cmd = get32(p->payload);
    c->a = p->size >= 8 ? get32(p->payload + 4) : 0;
    c->b = p->size >= 12 ? get32(p->payload + 8) : 0;
    c->c = p->size >= 16 ? get32(p->payload + 12) : 0;
    c->d = p->size >= 20 ? get32(p->payload + 16) : 0;
    return 0;
}

/* ---- QMI ---- */
int t970_qmi_parse(const uint8_t *data, size_t length, T970QmiMessage *m) {
    if (!data || !m || length < T970_QMI_HEADER) return -1;
    m->type = data[0]; m->txn = get16(data + 1); m->msg = get16(data + 3); m->tlv_length = get16(data + 5);
    if (m->tlv_length > length - T970_QMI_HEADER) return -2;
    m->tlv = data + T970_QMI_HEADER;
    return 0;
}
int t970_qmi_tlv(const T970QmiMessage *m, uint8_t type, const uint8_t **value, uint16_t *length) {
    for (uint32_t at = 0; at + 3 <= m->tlv_length;) {
        const uint8_t t = m->tlv[at];
        const uint16_t n = get16(m->tlv + at + 1);
        if (at + 3u + n > m->tlv_length) return -2;
        if (t == type) {if (value) *value = m->tlv + at + 3; if (length) *length = n; return 0;}
        at += 3u + n;
    }
    return -1;
}
size_t t970_sns_qmi_request(uint8_t *out, size_t cap, uint16_t txn, const uint8_t *request, size_t request_length) {
    const size_t tlv = 3 + 2 + request_length;
    if (!out || request_length > 0xffffu - 5 || cap < T970_QMI_HEADER + tlv) return 0;
    out[0] = T970_QMI_REQUEST; put16(out + 1, txn); put16(out + 3, T970_SNS_CLIENT_REQ); put16(out + 5, (uint16_t)tlv);
    out[7] = 0x01; put16(out + 8, (uint16_t)(2 + request_length)); put16(out + 10, (uint16_t)request_length);
    copy(out + 12, request, request_length);
    return T970_QMI_HEADER + tlv;
}
int t970_sns_qmi_response(const T970QmiMessage *m, uint16_t *result, uint16_t *error, uint64_t *client, int32_t *sns_result) {
    const uint8_t *v; uint16_t n;
    if (t970_qmi_tlv(m, 0x02, &v, &n) || n < 4) return -1;
    *result = get16(v); *error = get16(v + 2);
    *client = !t970_qmi_tlv(m, 0x10, &v, &n) && n >= 8 ? get64(v) : 0;
    *sns_result = !t970_qmi_tlv(m, 0x11, &v, &n) && n >= 4 ? (int32_t)get32(v) : -1;
    return 0;
}
int t970_sns_qmi_report(const T970QmiMessage *m, uint64_t *client, const uint8_t **event, uint16_t *event_length) {
    const uint8_t *v; uint16_t n;
    if (t970_qmi_tlv(m, 0x01, &v, &n) || n < 8) return -1;
    *client = get64(v);
    if (t970_qmi_tlv(m, 0x02, &v, &n) || n < 2 || get16(v) > n - 2) return -2;
    *event = v + 2; *event_length = get16(v);
    return 0;
}

/* ---- protobuf ---- */
typedef struct {uint8_t *p; size_t n, cap; int bad;} Writer;
static void wbyte(Writer *w, uint8_t b) {if (w->n < w->cap) w->p[w->n++] = b; else w->bad = 1;}
static void wvarint(Writer *w, uint64_t v) {do {uint8_t b = v & 0x7f; v >>= 7; wbyte(w, (uint8_t)(b | (v ? 0x80 : 0)));} while (v);}
static void wtag(Writer *w, unsigned field, unsigned wire) {wvarint(w, (uint64_t)field << 3 | wire);}
static void wfixed32(Writer *w, unsigned field, uint32_t v) {wtag(w, field, 5); for (int i = 0; i < 4; ++i) wbyte(w, (uint8_t)(v >> 8 * i));}
static void wfixed64(Writer *w, unsigned field, uint64_t v) {wtag(w, field, 1); for (int i = 0; i < 8; ++i) wbyte(w, (uint8_t)(v >> 8 * i));}
static void wbytes(Writer *w, unsigned field, const uint8_t *data, size_t n) {
    wtag(w, field, 2); wvarint(w, n);
    for (size_t i = 0; i < n; ++i) wbyte(w, data[i]);
}
static size_t done(Writer *w) {return w->bad ? 0 : w->n;}

size_t t970_sns_request(uint8_t *out, size_t cap, T970Suid suid, uint32_t msg_id, const uint8_t *payload, size_t payload_length) {
    uint8_t s[24], susp[8], req[1024];
    Writer ws = {s, 0, sizeof s, 0};
    wfixed64(&ws, 1, suid.low); wfixed64(&ws, 2, suid.high);
    Writer wp = {susp, 0, sizeof susp, 0};
    wtag(&wp, 1, 0); wvarint(&wp, 1);  /* client_proc_type = SNS_STD_CLIENT_PROCESSOR_APSS */
    wtag(&wp, 2, 0); wvarint(&wp, 0);  /* delivery_type = SNS_CLIENT_DELIVERY_WAKEUP */
    Writer wr = {req, 0, sizeof req, 0};
    if (payload_length) wbytes(&wr, 2, payload, payload_length);
    if (ws.bad || wp.bad || wr.bad) return 0;
    Writer w = {out, 0, cap, 0};
    wbytes(&w, 1, s, ws.n);
    wfixed32(&w, 2, msg_id);
    wbytes(&w, 3, susp, wp.n);
    wbytes(&w, 4, req, wr.n);
    return done(&w);
}
size_t t970_sns_suid_req(uint8_t *out, size_t cap, const char *data_type, int register_updates, int default_only) {
    size_t n = 0;
    while (data_type[n]) ++n;
    Writer w = {out, 0, cap, 0};
    wbytes(&w, 1, (const uint8_t *)data_type, n);
    wtag(&w, 2, 0); wvarint(&w, register_updates ? 1 : 0);
    wtag(&w, 3, 0); wvarint(&w, default_only ? 1 : 0);
    return done(&w);
}
uint32_t t970_f32_from_uint(uint32_t value) {
    if (!value) return 0;
    int top = 31;
    while (!(value >> top)) --top;
    const uint32_t mantissa = top > 23 ? (value >> (top - 23)) : (value << (23 - top));  /* truncates above 2^24 */
    return (uint32_t)(127 + top) << 23 | (mantissa & 0x7fffffu);
}
int32_t t970_f32_scale(uint32_t bits, int32_t scale) {
    const int negative = (bits >> 31) != (scale < 0);
    const uint64_t s = scale < 0 ? (uint64_t)(-(int64_t)scale) : (uint64_t)scale;
    const int exponent = (int)((bits >> 23) & 0xff);
    if (exponent == 0 || !s) return 0;  /* zero or denormal */
    if (exponent == 0xff) return negative ? INT32_MIN : INT32_MAX;
    const uint64_t mantissa = (bits & 0x7fffffu) | 0x800000u;  /* value = mantissa * 2^(exponent - 150) */
    const int shift = exponent - 150;
    uint64_t magnitude;
    if (shift >= 0) {
        if (shift > 20) return negative ? INT32_MIN : INT32_MAX;
        magnitude = (mantissa * s) << shift;
    } else if (shift > -64) {
        const uint64_t product = mantissa * s;  /* < 2^24 * 2^31 */
        magnitude = (product + (1ull << (-shift - 1))) >> -shift;
    } else {
        magnitude = 0;
    }
    if (magnitude > 0x7fffffffull) return negative ? INT32_MIN : INT32_MAX;
    return negative ? -(int32_t)magnitude : (int32_t)magnitude;
}
size_t t970_sns_sensor_config(uint8_t *out, size_t cap, uint32_t sample_rate_hz) {
    Writer w = {out, 0, cap, 0};
    wfixed32(&w, 1, t970_f32_from_uint(sample_rate_hz));
    return done(&w);
}

/* One field: field number, wire type, the varint/fixed value or the bytes of a length-delimited one. */
typedef struct {unsigned field, wire; uint64_t value; const uint8_t *bytes; size_t length;} Field;
static int rvarint(const uint8_t *d, size_t n, size_t *at, uint64_t *v) {
    uint64_t r = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*at >= n) return -1;
        const uint8_t b = d[(*at)++];
        r |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {*v = r; return 0;}
    }
    return -1;
}
/* 1 when a field was read, 0 at the end, < 0 when malformed. */
static int next(const uint8_t *d, size_t n, size_t *at, Field *f) {
    if (*at >= n) return 0;
    uint64_t key;
    if (rvarint(d, n, at, &key)) return -1;
    f->field = (unsigned)(key >> 3); f->wire = (unsigned)(key & 7); f->bytes = 0; f->length = 0; f->value = 0;
    switch (f->wire) {
    case 0: return rvarint(d, n, at, &f->value) ? -1 : 1;
    case 1: if (n - *at < 8) return -1; f->value = get64(d + *at); *at += 8; return 1;
    case 5: if (n - *at < 4) return -1; f->value = get32(d + *at); *at += 4; return 1;
    case 2: {
        uint64_t len;
        if (rvarint(d, n, at, &len) || len > n - *at) return -1;
        f->bytes = d + *at; f->length = (size_t)len; *at += (size_t)len;
        return 1;
    }
    default: return -1;
    }
}
static int suid(const uint8_t *d, size_t n, T970Suid *s) {
    size_t at = 0; Field f; int rc;
    s->low = s->high = 0;
    while ((rc = next(d, n, &at, &f)) > 0) {
        if (f.field == 1 && f.wire == 1) s->low = f.value;
        else if (f.field == 2 && f.wire == 1) s->high = f.value;
    }
    return rc;
}
int t970_sns_events(const uint8_t *message, size_t length, T970SnsEach each, void *context) {
    size_t at = 0; Field f; int rc, count = 0;
    T970Suid from = {0, 0};
    /* The suid comes first on the wire, but nothing requires it: collect the events after a first pass for it. */
    while ((rc = next(message, length, &at, &f)) > 0)
        if (f.field == 1 && f.wire == 2 && suid(f.bytes, f.length, &from) < 0) return -2;
    if (rc < 0) return -1;
    at = 0;
    while ((rc = next(message, length, &at, &f)) > 0) {
        if (f.field != 2 || f.wire != 2) continue;
        size_t e = 0; Field g; int erc;
        uint32_t id = 0; uint64_t timestamp = 0; const uint8_t *payload = 0; size_t plen = 0;
        while ((erc = next(f.bytes, f.length, &e, &g)) > 0) {
            if (g.field == 1 && g.wire == 5) id = (uint32_t)g.value;
            else if (g.field == 2 && g.wire == 1) timestamp = g.value;
            else if (g.field == 3 && g.wire == 2) {payload = g.bytes; plen = g.length;}
        }
        if (erc < 0) return -3;
        if (each) each(context, from, id, timestamp, payload, plen);
        ++count;
    }
    return rc < 0 ? -1 : count;
}
int t970_sns_suid_event(const uint8_t *payload, size_t length, char *type, size_t type_cap, T970Suid *suids, unsigned max) {
    size_t at = 0; Field f; int rc; unsigned count = 0;
    if (type_cap) type[0] = 0;
    while ((rc = next(payload, length, &at, &f)) > 0) {
        if (f.field == 1 && f.wire == 2 && type_cap) {
            size_t n = f.length < type_cap - 1 ? f.length : type_cap - 1;
            for (size_t i = 0; i < n; ++i) type[i] = (char)f.bytes[i];
            type[n] = 0;
        } else if (f.field == 2 && f.wire == 2) {
            if (count < max && suid(f.bytes, f.length, &suids[count]) < 0) return -2;
            ++count;
        }
    }
    if (rc < 0) return -1;
    return (int)(count < max ? count : max);
}
int t970_sns_sensor_event(const uint8_t *payload, size_t length, uint32_t *data, unsigned max, int32_t *status) {
    size_t at = 0; Field f; int rc; unsigned count = 0;
    *status = -1;
    while ((rc = next(payload, length, &at, &f)) > 0) {
        if (f.field == 1 && f.wire == 5) {if (count < max) data[count] = (uint32_t)f.value; ++count;}
        else if (f.field == 1 && f.wire == 2) {
            for (size_t i = 0; i + 4 <= f.length; i += 4) {if (count < max) data[count] = get32(f.bytes + i); ++count;}
        } else if (f.field == 2 && f.wire == 0) *status = (int32_t)f.value;
    }
    if (rc < 0) return -1;
    return (int)(count < max ? count : max);
}
