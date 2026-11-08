/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The sensor hub (SSC, on the SLPI) as an APPS client reaches it: QRTR packets on the GLINK "IPCRTR" channel, QMI messages of the
 * sensor client service (service 400) inside them, and the protobuf messages of the SSC client API inside those. Pure encoders and
 * decoders over caller buffers: no allocation, no hardware, so the kernel driver (T970Sensors.cpp) and the host tests share them.
 *
 * Message layouts are the public ones of the Linux QRTR (include/uapi/linux/qrtr.h, net/qrtr), the QMI wire format (TLVs after a
 * 7-byte header) and the SSC client protos as libssc (postmarketOS) uses them: sns_client_request_msg, sns_client_event_msg,
 * sns_suid_req/event and sns_std_sensor_config/event. */
#ifndef T970_SSC_H
#define T970_SSC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- QRTR (version 1 header, 32 bytes, all little endian) ---- */
#define T970_QRTR_HEADER 32u
#define T970_QRTR_PORT_CTRL 0xfffffffeu
#define T970_QRTR_NODE_BCAST 0xffffffffu
#define T970_QRTR_NODE_APPS 1u
enum {
    T970_QRTR_DATA = 1, T970_QRTR_HELLO = 2, T970_QRTR_BYE = 3, T970_QRTR_NEW_SERVER = 4, T970_QRTR_DEL_SERVER = 5,
    T970_QRTR_DEL_CLIENT = 6, T970_QRTR_RESUME_TX = 7, T970_QRTR_EXIT = 8, T970_QRTR_PING = 9, T970_QRTR_NEW_LOOKUP = 10,
    T970_QRTR_DEL_LOOKUP = 11,
};
typedef struct {
    uint32_t type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
    const uint8_t *payload;  /* size bytes, inside the parsed packet */
} T970QrtrPacket;
/* A control packet's body (NEW_SERVER/DEL_SERVER: service, instance, node, port; DEL_CLIENT/RESUME_TX: node, port). */
typedef struct {uint32_t cmd, a, b, c, d;} T970QrtrCtrl;
/* Writes header + payload (padded to 4) into out; returns the bytes written or 0 when cap is too small. */
size_t t970_qrtr_pack(uint8_t *out, size_t cap, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node,
                      uint32_t dst_port, uint32_t confirm_rx, const void *payload, uint32_t size);
/* A control packet from our control port (cmd is also the type); words a..d as in T970QrtrCtrl. */
size_t t970_qrtr_ctrl(uint8_t *out, size_t cap, uint32_t cmd, uint32_t src_node, uint32_t dst_node, uint32_t a, uint32_t b,
                      uint32_t c, uint32_t d);
/* 0 when data holds a version 1 packet whose payload fits; fills p. */
int t970_qrtr_parse(const uint8_t *data, size_t length, T970QrtrPacket *p);
int t970_qrtr_parse_ctrl(const T970QrtrPacket *p, T970QrtrCtrl *c);

/* ---- QMI ---- */
#define T970_QMI_REQUEST 0u
#define T970_QMI_RESPONSE 2u
#define T970_QMI_INDICATION 4u
#define T970_QMI_HEADER 7u
#define T970_SNS_SERVICE 400u
#define T970_SNS_CLIENT_REQ 0x20u
#define T970_SNS_CLIENT_REPORT_IND 0x21u
#define T970_SNS_CLIENT_JUMBO_REPORT_IND 0x22u
typedef struct {
    uint8_t type;
    uint16_t txn, msg;
    const uint8_t *tlv;
    uint16_t tlv_length;
} T970QmiMessage;
int t970_qmi_parse(const uint8_t *data, size_t length, T970QmiMessage *m);
/* The value of TLV type in m; 0 when found. */
int t970_qmi_tlv(const T970QmiMessage *m, uint8_t type, const uint8_t **value, uint16_t *length);
/* SNS_CLIENT_REQ with the protobuf request in TLV 1 (u16 count + bytes); returns the bytes written, 0 on overflow. */
size_t t970_sns_qmi_request(uint8_t *out, size_t cap, uint16_t txn, const uint8_t *request, size_t request_length);
/* The response: QMI result/error (TLV 2), client id (TLV 0x10, 0 if absent), SSC result (TLV 0x11, -1 if absent). */
int t970_sns_qmi_response(const T970QmiMessage *m, uint16_t *result, uint16_t *error, uint64_t *client, int32_t *sns_result);
/* A report indication (normal or jumbo): the client id and the protobuf event message. */
int t970_sns_qmi_report(const T970QmiMessage *m, uint64_t *client, const uint8_t **event, uint16_t *event_length);

/* ---- SSC client protobuf messages ---- */
#define T970_SUID_SENSOR_LOW 0xababababababababull
#define T970_SUID_SENSOR_HIGH 0xababababababababull
#define T970_SNS_SUID_REQ 512u
#define T970_SNS_SUID_EVENT 768u
#define T970_SNS_STD_ATTR_REQ 1u
#define T970_SNS_STD_FLUSH_EVENT 1024u
#define T970_SNS_STD_SENSOR_CONFIG 513u
#define T970_SNS_STD_SENSOR_EVENT 1025u
#define T970_SNS_STD_SENSOR_CONFIG_EVENT 1026u
#define T970_SNS_STD_DISABLE 10u
#define T970_SNS_STD_ERROR_EVENT 6u
typedef struct {uint64_t low, high;} T970Suid;
/* sns_client_request_msg for suid/msg_id with a sns_std_request carrying payload (may be empty); 0 on overflow. */
size_t t970_sns_request(uint8_t *out, size_t cap, T970Suid suid, uint32_t msg_id, const uint8_t *payload, size_t payload_length);
/* sns_suid_req {data_type, register_updates, default_only} */
size_t t970_sns_suid_req(uint8_t *out, size_t cap, const char *data_type, int register_updates, int default_only);
/* sns_std_sensor_config {sample_rate}; the rate in whole Hz (the kernel builds without floating point registers: floats are carried
 * as their IEEE-754 bit patterns and converted with integer arithmetic). */
size_t t970_sns_sensor_config(uint8_t *out, size_t cap, uint32_t sample_rate_hz);
/* IEEE-754 single bits of a small non-negative integer, and round(f * scale) of single bits f (saturating to the int32 range). */
uint32_t t970_f32_from_uint(uint32_t value);
int32_t t970_f32_scale(uint32_t bits, int32_t scale);
/* Walks a sns_client_event_msg: calls each(context, suid, msg_id, timestamp, payload, length) per event; returns the event count
 * or a negative value when the message is malformed. */
typedef void (*T970SnsEach)(void *context, T970Suid suid, uint32_t msg_id, uint64_t timestamp, const uint8_t *payload,
                            size_t length);
int t970_sns_events(const uint8_t *message, size_t length, T970SnsEach each, void *context);
/* sns_suid_event: copies data_type (NUL terminated, truncated to type_cap) and up to max suids; returns the suid count or < 0. */
int t970_sns_suid_event(const uint8_t *payload, size_t length, char *type, size_t type_cap, T970Suid *suids, unsigned max);
/* sns_std_sensor_event: up to max floats of data (packed or not, as bit patterns) and the status (-1 if absent); returns the float
 * count or < 0. */
int t970_sns_sensor_event(const uint8_t *payload, size_t length, uint32_t *data, unsigned max, int32_t *status);

#ifdef __cplusplus
}
#endif
#endif
