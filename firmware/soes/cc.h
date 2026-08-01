/*
 * Compiler/platform shim for SOES on Zephyr.
 *
 * SOES ships soes/include/sys/gcc/cc.h, but that one pulls in <sys/param.h>
 * and <machine/endian.h>, neither of which exists under Zephyr's picolibc.
 * This replacement provides the same handful of definitions from Zephyr's
 * own headers instead.
 */
#ifndef CC_H
#define CC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define CC_PACKED_BEGIN
#define CC_PACKED_END
#define CC_PACKED __attribute__((packed))
#define CC_ALIGNED(n) __attribute__((aligned(n)))

#define CC_ASSERT(exp) __ASSERT_NO_MSG(exp)
#define CC_STATIC_ASSERT(exp, msg) _Static_assert(exp, msg)

/*
 * SOES logs through DPRINT. This uses printk rather than LOG_INF because
 * Zephyr's logging macros need a LOG_MODULE_REGISTER in every translation
 * unit that calls them, and SOES's own sources have none — adding them would
 * mean patching vendored code.
 */
#define DPRINT(...) printk(__VA_ARGS__)
#define DEBUG_ASSERT(exp) __ASSERT_NO_MSG(exp)

/* The ESP32-S3 is little-endian, like every target SOES supports, so these
 * are identity conversions. Spelled out rather than assumed so a big-endian
 * port fails loudly at compile time instead of silently swapping data. */
#if defined(CONFIG_BIG_ENDIAN)
#error "SOES here assumes a little-endian target"
#endif

/*
 * SOES selects its packed register and mailbox struct layouts on this macro.
 * Without it every bitfield resolves to the wrong branch and the build fails
 * with a long list of "has no member named ECsm / mbxcnt / MBXstat".
 */
#define EC_LITTLE_ENDIAN

#define htoes(x) (x)
#define etohs(x) (x)
#define htoel(x) (x)
#define etohl(x) (x)
#define htoell(x) (x)
#define etohll(x) (x)

#ifdef __cplusplus
}
#endif

#endif /* CC_H */
