/*
 * Chihiro JVS I/O Board Emulation (Sega 837-13551)
 *
 * JVS (JAMMA Video Standard) I/O board used by Chihiro, Naomi, Triforce.
 * Protocol references: MAME jvsdev.cpp, Flycast maple_jvs.cpp,
 * Lindbergh Loader jvs.c, Dolphin JVSIO.cpp.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "chihiro-jvs.h"
#include <string.h>

#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))

ChihiroJVSState *chihiro_jvs_global = NULL;

static const char board_id[] =
    "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

static const uint8_t capabilities[] = {
    0x01, 0x02, 0x0D, 0x00,   /* Switch: 2 players, 13 buttons each */
    0x02, 0x02, 0x00, 0x00,   /* Coin: 2 slots */
    0x03, 0x08, 0x10, 0x00,   /* Analog: 8 channels, 16-bit */
    0x12, 0x06, 0x00, 0x00,   /* GP output: 6 channels */
    0x00                       /* Terminator */
};

void chihiro_jvs_init(ChihiroJVSState *s)
{
    memset(s, 0, sizeof(*s));
    s->sense = 3;
    for (int i = 0; i < JVS_MAX_ANALOG; i++) {
        s->analog[i] = 0x8000;
    }
}

static int jvs_unescape(const uint8_t *src, int src_len,
                         uint8_t *dst, int dst_max)
{
    int di = 0;
    for (int si = 0; si < src_len && di < dst_max; si++) {
        if (src[si] == JVS_ESCAPE && si + 1 < src_len) {
            dst[di++] = src[++si] + 1;
        } else {
            dst[di++] = src[si];
        }
    }
    return di;
}

static int jvs_escape(const uint8_t *src, int src_len,
                       uint8_t *dst, int dst_max)
{
    int di = 0;
    for (int si = 0; si < src_len && di < dst_max - 1; si++) {
        if (src[si] == JVS_SYNC || src[si] == JVS_ESCAPE) {
            dst[di++] = JVS_ESCAPE;
            dst[di++] = src[si] - 1;
        } else {
            if (di < dst_max) dst[di++] = src[si];
        }
    }
    return di;
}

/*
 * Handle a single JVS command from the unescaped data stream.
 * Returns the number of input bytes consumed (command + params).
 * Appends report bytes to resp[*rpos].
 */
static int jvs_handle_command(ChihiroJVSState *s,
                               const uint8_t *cmd, int cmd_len,
                               uint8_t *resp, int *rpos, int rmax)
{
    if (cmd_len < 1) return 0;
    int rp = *rpos;

#define PUT(b) do { if (rp < rmax) resp[rp++] = (b); } while(0)

    switch (cmd[0]) {
    case 0xF0: /* Reset */
        if (cmd_len < 2) return 1;
        s->reset_count++;
        if (s->reset_count >= 2) {
            s->device_id = 0;
            s->sense = 3;
            s->reset_count = 0;
        }
        *rpos = rp;
        return 2;

    case 0xF1: /* Set Device ID */
        if (cmd_len < 2) return 1;
        s->device_id = cmd[1];
        s->sense = 0;
        s->reset_count = 0;
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 2;

    case 0x10: /* Request Board ID */
        PUT(JVS_REPORT_OK);
        for (int i = 0; i <= (int)strlen(board_id); i++) {
            PUT(board_id[i]);
        }
        *rpos = rp;
        return 1;

    case 0x11: /* Command Format Version */
        PUT(JVS_REPORT_OK);
        PUT(0x11);
        *rpos = rp;
        return 1;

    case 0x12: /* JVS Revision */
        PUT(JVS_REPORT_OK);
        PUT(0x20);
        *rpos = rp;
        return 1;

    case 0x13: /* Communication Version */
        PUT(JVS_REPORT_OK);
        PUT(0x10);
        *rpos = rp;
        return 1;

    case 0x14: /* Feature Check */
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < (int)sizeof(capabilities); i++) {
            PUT(capabilities[i]);
        }
        *rpos = rp;
        return 1;

    case 0x15: { /* Convey Main Board ID */
        int consumed = 1;
        while (consumed < cmd_len && cmd[consumed] != '\0') {
            consumed++;
        }
        if (consumed < cmd_len) consumed++; /* skip the NUL */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return consumed;
    }

    case 0x20: { /* Read Switch Inputs */
        if (cmd_len < 3) return 1;
        int players = cmd[1];
        int bytes_per = cmd[2];
        PUT(JVS_REPORT_OK);
        PUT(s->system_switches);
        for (int p = 0; p < players && p < JVS_MAX_PLAYERS; p++) {
            for (int b = 0; b < bytes_per && b < 2; b++) {
                PUT(s->player_switches[p][b]);
            }
        }
        *rpos = rp;
        return 3;
    }

    case 0x21: { /* Read Coin Inputs */
        if (cmd_len < 2) return 1;
        int slots = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < slots && i < JVS_MAX_COINS; i++) {
            PUT((s->coin_count[i] >> 8) & 0x3F);
            PUT(s->coin_count[i] & 0xFF);
        }
        *rpos = rp;
        return 2;
    }

    case 0x22: { /* Read Analog Inputs */
        if (cmd_len < 2) return 1;
        int channels = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < channels && i < JVS_MAX_ANALOG; i++) {
            PUT((s->analog[i] >> 8) & 0xFF);
            PUT(s->analog[i] & 0xFF);
        }
        *rpos = rp;
        return 2;
    }

    case 0x26: { /* Read Misc Switch Inputs */
        if (cmd_len < 2) return 1;
        int nbytes = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < nbytes; i++) PUT(0x00);
        *rpos = rp;
        return 2;
    }

    case 0x2E: { /* Read Payout Hopper Status */
        if (cmd_len < 2) return 1;
        int slots = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < slots; i++) {
            PUT(0); PUT(0); PUT(0); PUT(0);
        }
        *rpos = rp;
        return 2;
    }

    case 0x2F: /* Retransmit Data */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 1;

    case 0x30:
    case 0x31: { /* Coin Decrease/Increase */
        if (cmd_len < 4) return 1;
        int slot = cmd[1] - 1;
        uint16_t count = (cmd[2] << 8) | cmd[3];
        if (slot >= 0 && slot < JVS_MAX_COINS) {
            if (cmd[0] == 0x30) {
                if (s->coin_count[slot] >= count)
                    s->coin_count[slot] -= count;
            } else {
                s->coin_count[slot] += count;
            }
        }
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 4;
    }

    case 0x32: { /* General Purpose Output */
        if (cmd_len < 2) return 1;
        int banks = cmd[1];
        int consumed = 2;
        for (int i = 0; i < banks && consumed < cmd_len; i++, consumed++) {
            if (i < JVS_MAX_GP_OUTPUT)
                s->gp_output[i] = cmd[consumed];
        }
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return consumed;
    }

    case 0x33: { /* Analog Output */
        if (cmd_len < 2) return 1;
        int channels = cmd[1];
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 2 + channels * 2;
    }

    case 0x34: { /* Character Output */
        if (cmd_len < 2) return 1;
        int nbytes = cmd[1];
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 2 + nbytes;
    }

    case 0x36: /* Payout Subtraction Output */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 4;

    case 0x37: /* General Purpose Output 2 */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 3;

    default:
        /* Unknown command — return InvalidParameter (NOT UnsupportedCommand,
         * which would trigger Error 11 in the game) */
        PUT(JVS_REPORT_PARAM);
        *rpos = rp;
        return 1;
    }
#undef PUT
}

int chihiro_jvs_process(ChihiroJVSState *s,
                         const uint8_t *in, int in_len,
                         uint8_t *out, int out_max)
{
    if (in_len < 3 || in[0] != JVS_SYNC) return 0;

    uint8_t target = in[1];
    s->last_target = target;
    int escaped_count = in[2];

    /* JVS over Chihiro USB is a raw byte stream — escape encoding (0xD0)
     * is a physical RS-485 layer concern and is NOT used over USB. */
    const uint8_t *raw = in + 3;
    int raw_len = in_len - 3;

    if (raw_len < escaped_count) {
        /* Incomplete packet — but try to process what we have */
    }

    /* Verify checksum: sum of (target + count + all_data_bytes) & 0xFF */
    int data_len = (raw_len >= escaped_count) ? escaped_count - 1 : raw_len - 1;
    if (data_len < 0) data_len = 0;

    uint8_t csum = target + escaped_count;
    for (int i = 0; i < data_len; i++) csum += raw[i];
    if (raw_len >= escaped_count && raw[escaped_count - 1] != (csum & 0xFF)) {
        printf("[%07lld] JVS: checksum error (got 0x%02X, expected 0x%02X)\n",
               TS_MS, raw[escaped_count - 1], csum & 0xFF);
    }

    /* Broadcast: Reset gets no response, but Set ID does (claiming device responds) */
    if (target == JVS_BROADCAST) {
        if (data_len >= 1 && raw[0] == 0xF1) {
            /* Set ID on broadcast — fall through to addressed handler so we respond */
        } else {
            const uint8_t *cmd = raw;
            int remaining = data_len;
            while (remaining > 0) {
                int consumed = jvs_handle_command(s, cmd, remaining,
                                                  NULL, &(int){0}, 0);
                if (consumed <= 0) break;
                cmd += consumed;
                remaining -= consumed;
            }
            return 0;
        }
    }

    /* Addressed packet — must match our device_id (broadcast Set ID also passes) */
    if (target != JVS_BROADCAST && target != s->device_id && s->device_id != 0) return 0;

    /* Process commands and build response payload */
    uint8_t payload[256];
    int ppos = 0;
    payload[ppos++] = JVS_STATUS_OK;

    const uint8_t *cmd = raw;
    int remaining = data_len;
    while (remaining > 0) {
        int consumed = jvs_handle_command(s, cmd, remaining,
                                          payload, &ppos, sizeof(payload));
        if (consumed <= 0) break;
        cmd += consumed;
        remaining -= consumed;
    }

    /* Build framed response: SYNC + host_addr + count + payload + checksum
     * No escape encoding — USB transport uses raw bytes. */
    uint8_t frame[256];
    int fpos = 0;
    uint8_t resp_count = ppos + 1; /* payload + checksum */

    uint8_t resp_csum = JVS_HOST_ADDR + resp_count;
    for (int i = 0; i < ppos; i++) resp_csum += payload[i];

    frame[fpos++] = JVS_SYNC;
    frame[fpos++] = JVS_HOST_ADDR;
    frame[fpos++] = resp_count;
    memcpy(frame + fpos, payload, ppos);
    fpos += ppos;
    frame[fpos++] = resp_csum & 0xFF;

    int out_len = (fpos < out_max) ? fpos : out_max;
    memcpy(out, frame, out_len);

    if(0) printf("[%07lld] JVS: cmd=%02X → resp %d bytes (sense=%d id=%d)\n",
           TS_MS, (data_len > 0) ? raw[0] : 0, out_len, s->sense, s->device_id);

    return out_len;
}
