/* hakuX native core — QEMU's virtual clock, for the Chihiro sources' logs
 * and timeouts: host milliseconds since the baseboard came up. */
#ifndef NC_CHIHIRO_SHIM_TIMER_H
#define NC_CHIHIRO_SHIM_TIMER_H

#include <stdint.h>

enum { QEMU_CLOCK_VIRTUAL };
int64_t qemu_clock_get_ms(int clock);

#endif
