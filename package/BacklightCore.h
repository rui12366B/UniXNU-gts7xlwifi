// SPDX-License-Identifier: BSD-2-Clause-Patent
// The packet and level arithmetic for this board's panel brightness, kept out of the driver so a
// host test can check it against the vendor kernel's own rules without a device.
//
// gts7xlwifi: the panel is ss_dsi_panel_S6TUUM3_AMSA24VU01_WQXGA (live FDT bootargs
// dsi_display0=ss_dsi_panel_S6TUUM3_AMSA24VU01_WQXGA), dsi_cmd_mode, bl_ctrl_dcs, split over DSI0 and
// DSI1. Samsung's driver (techpack/display/msm/samsung/S6TUUM3_AMSA24VU01/ss_dsi_panel_S6TUUM3_AMSA24VU01.c
// ss_brightness_gamma_mode2_normal, and samsung,gamma_mode2_normal_tx_cmds_revA in
// dsi_panel_S6TUUM3_AMSA24VU01_wqxga_octa_cmd.dtsi) sends three DCS long writes for every normal level:
//   68 19 20 00 14
//   51 DBV[7:0] DBV[10:8]          (low byte first: tx_buf[1] = get_bit(dbv, 0, 8), tx_buf[2] = get_bit(dbv, 8, 3))
//   53 20                          (0x28 turns the panel's own smooth dimming on; this driver fades itself)
// The level is the DBV (gm2_wrdisbv) of samsung,candela_map_table_revA: 10 at 2 nits up to 2047 at 420 nits
// (bl_level 0..255). Higher levels are the HBM table, which needs other commands and is not used here.
#ifndef BACKLIGHT_CORE_H
#define BACKLIGHT_CORE_H
#include <stdint.h>

enum {
  BL_MAX_LEVEL = 2047,       // candela_map_table_revA bl_level 255: DBV 2047, 420 nits
  BL_MIN_LEVEL = 10,         // candela_map_table_revA bl_level 0: DBV 10, 2 nits
  BL_DEFAULT_LEVEL = 1029,   // bl_level 128 (about 210 nits), the middle of the table
  BL_DCS_BRIGHTNESS = 0x51, BL_DCS_BRIGHTNESS_GET = 0x52, BL_DCS_CONTROL = 0x53,
  BL_DCS_DISPLAY_ON = 0x29, BL_DCS_DISPLAY_OFF = 0x28,
  BL_DCS_SLEEP_OUT = 0x11, BL_DCS_SLEEP_IN = 0x10,
  BL_MAX_PACKET_WORDS = 4,   // header plus up to 12 payload bytes
};

// The four header bytes of a MIPI packet, in order, as dsi_ctrl_copy_and_pad_cmd lays them out
// (techpack/display/msm/dsi/dsi_ctrl.c): byte 0 at bits 7:0, byte 1 at 15:8, byte 2 (the
// data type) at 23:16, byte 3 (the flags) at 31:24. Flags: bit 6 payload present, bit 7 last
// command, bit 5 turn the lane around for a read.
enum {BL_FLAG_PAYLOAD = 1u << 30, BL_FLAG_LAST = 1u << 31, BL_FLAG_BTA = 1u << 29};

// A DCS long write: the word count (command byte plus parameters) in header bytes 0..1.
static inline uint32_t bl_long_write_header(uint32_t bytes) {
  return BL_FLAG_PAYLOAD | BL_FLAG_LAST | (0x39u << 16) | (bytes & 0xffffu);
}
// A DCS short write with no parameter: 0x80050000 | command. The command byte is header byte 0.
static inline uint32_t bl_short_write(uint8_t command) {
  return BL_FLAG_LAST | (0x05u << 16) | (uint32_t)command;
}
// SET_MAXIMUM_RETURN_PACKET_SIZE (0x37) with the size as its one parameter.
static inline uint32_t bl_max_return_header(uint32_t size) {
  return BL_FLAG_PAYLOAD | BL_FLAG_LAST | (0x37u << 16) | 2u | ((size & 0xffu) << 8);
}
static inline uint32_t bl_max_return_payload(uint32_t size) {return (size >> 8) & 0xffu;}
// A DCS read: 0xA0060000 | command, with the BTA flag so the panel turns the lane around.
static inline uint32_t bl_read_header(uint8_t command) {
  return BL_FLAG_BTA | BL_FLAG_LAST | (0x06u << 16) | (uint32_t)command;
}

// A whole DCS long write: header, then the bytes packed little endian into words, padded with 0xff to a
// whole word as dsi_ctrl_copy_and_pad_cmd does. Returns the word count, 0 when it does not fit.
static inline unsigned bl_pack_long_write(const uint8_t *bytes, unsigned count, uint32_t *words, unsigned room) {
  const unsigned payload_words = (count + 3u) / 4u;
  if (!count || 1u + payload_words > room) return 0;
  words[0] = bl_long_write_header(count);
  for (unsigned w = 0; w < payload_words; ++w) {
    uint32_t word = 0;
    for (unsigned b = 0; b < 4; ++b) {
      const unsigned i = w * 4u + b;
      word |= (uint32_t)(i < count ? bytes[i] : 0xffu) << (8u * b);
    }
    words[1 + w] = word;
  }
  return 1u + payload_words;
}

// The DCS 0x51 parameters for a DBV, in the panel's order: [7:0] first, then [10:8].
static inline void bl_brightness_bytes(uint32_t level, uint8_t out[3]) {
  out[0] = BL_DCS_BRIGHTNESS;
  out[1] = (uint8_t)(level & 0xffu);
  out[2] = (uint8_t)((level >> 8) & 0x7u);
}

// A level the panel accepts: inside the normal (non-HBM) DBV range. Zero is the minimum, not display
// off: the panel's power sequence (samsung,display_on_tx_cmds_revA) is more than a sleep-out.
static inline uint32_t bl_clamp_level(uint32_t level) {
  if (level > (uint32_t)BL_MAX_LEVEL) return BL_MAX_LEVEL;
  if (level < (uint32_t)BL_MIN_LEVEL) return BL_MIN_LEVEL;
  return level;
}

// The two bytes a DCS 0x52 read returns, in the order the panel sends them, from the word the controller
// latched. get_cmd_read_data reads RDBK_DATA highest word first and byte-swaps, so the caller passes the
// already-swapped word. Recorded raw: the byte order of this DDI's read-back is not documented.
static inline uint32_t bl_decode_brightness(uint32_t word) {
  return ((word >> 8) & 0xffu) << 8 | (word & 0xffu);
}
#endif
