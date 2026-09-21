#ifndef CHIHIRO_JVS_H
#define CHIHIRO_JVS_H

#include <stdint.h>
#include <stdbool.h>

#define JVS_MAX_PLAYERS    2
#define JVS_MAX_COINS      2
#define JVS_MAX_ANALOG     8
#define JVS_MAX_GP_OUTPUT  6

#define JVS_SYNC           0xE0
#define JVS_ESCAPE         0xD0
#define JVS_BROADCAST      0xFF
#define JVS_HOST_ADDR      0x00

#define JVS_STATUS_OK      0x01
#define JVS_STATUS_UNSUP   0x02
#define JVS_STATUS_CSUM    0x03

#define JVS_REPORT_OK      0x01
#define JVS_REPORT_PARAM   0x02
#define JVS_REPORT_DATA    0x03

typedef struct {
    uint8_t  device_id;
    uint8_t  sense;                    /* 3=unassigned, 0=addressed */
    uint16_t coin_count[JVS_MAX_COINS];
    uint8_t  reset_count;              /* must reach 2 for actual reset */

    uint8_t  system_switches;          /* test=0x80, tilt1=0x40 ... */
    uint8_t  player_switches[JVS_MAX_PLAYERS][2];
    uint16_t analog[JVS_MAX_ANALOG];   /* 16-bit, 0x8000 = center */
    uint8_t  gp_output[JVS_MAX_GP_OUTPUT];

    uint8_t  last_target;              /* target from last JVS request */

    uint8_t  response[256];
    int      response_len;
} ChihiroJVSState;

extern ChihiroJVSState *chihiro_jvs_global;

void chihiro_jvs_init(ChihiroJVSState *s);

/*
 * Process a raw JVS frame (starting with 0xE0 sync).
 * Returns the number of bytes written to out[], or 0 if no response
 * (broadcast commands like Reset produce no response).
 */
int  chihiro_jvs_process(ChihiroJVSState *s,
                          const uint8_t *in, int in_len,
                          uint8_t *out, int out_max);

#endif
