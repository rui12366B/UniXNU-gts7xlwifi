/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* What Linux v7.0.14 ath11k needs beyond the Ath12kWifi shim (UniXNU-sheng's ath12k build), for gts7xlwifi. Adapted from
 * Chengdong-Yu/pong-wifi ath11k/compat/PongCompat.h (d2e33ca6, BSD-2-Clause-Patent). Included first by every ath11k object. */
#ifndef T970_ATH11K_COMPAT_H
#define T970_ATH11K_COMPAT_H
#include "lx_pci.h"
typedef unsigned long kernel_ulong_t;
#define swab32(x) __builtin_bswap32((u32)(x))
#define prefetch(p) __builtin_prefetch(p)
/* ath11k's non-coherent rings (reo queues, CE pipes) come from the same DMA window as coherent ones: the window is mapped
 * uncached on the CPU side, so no cache maintenance is skipped by treating them alike. */
#define dma_alloc_noncoherent(d, n, h, dir, f) lx_dma_alloc(d, n, h)
#define dma_free_noncoherent(d, n, p, h, dir) lx_dma_free(d, n, p, h)
static inline u32 *memset32(u32 *dst, u32 value, size_t count)
{ u32 *result = dst; while (count--) *dst++ = value; return result; }
/* The PCI path has no device tree node: no reserved-memory region, no fixed firmware memory, no calibration variant. */
static inline struct device_node *of_find_node_by_type(struct device_node *node, const char *type)
{ (void)node; (void)type; return NULL; }
static inline bool of_property_present(const struct device_node *node, const char *name)
{ (void)node; (void)name; return false; }
static inline bool of_machine_is_compatible(const char *name)
{ return name && (!strcmp(name, "qcom,sm8250") || !strcmp(name, "samsung,gts7xlwifi")); }
/* No firmware-node address: the radio's address comes from the chip or nw_board_mac (package/T970Ath11k.c). */
static inline int device_get_mac_address(struct device *device, u8 *address)
{ (void)device; memset(address, 0, 6); return -ENODEV; }
#ifndef resource_size
#define resource_size(r) ((r)->end - (r)->start + 1)
#endif
static inline int of_reserved_mem_region_to_resource(struct device_node *node, int i, struct resource *r)
{ (void)node; (void)i; (void)r; return -ENODEV; }
static inline void *devm_ioremap(struct device *device, phys_addr_t pa, size_t bytes)
{ (void)device; return lx_ioremap(pa, bytes); }
/* debug.c's hex dump: bytes of one row as "xx xx ..." (group size 1, no ASCII column), what ath11k_dbg_dump asks for. */
static inline int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *line, size_t size, bool ascii)
{
	(void)groupsize; (void)ascii; const u8 *p = buf; size_t o = 0;
	if (len > (size_t)rowsize) len = rowsize;
	if (size) line[0] = 0;
	for (size_t i = 0; i < len && o + 4 <= size; i++) o += snprintf(line + o, size - o, i ? " %02x" : "%02x", p[i]);
	return (int)o;
}
#endif
