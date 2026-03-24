/*
 * QEMU libpcap network client stub
 *
 * Copyright (C) 2026 X1 BOX contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "net/clients.h"
#include "qapi/error.h"

int net_init_pcap(const Netdev *netdev, const char *name, NetClientState *peer,
                  Error **errp)
{
    (void)netdev;
    (void)name;
    (void)peer;
    error_setg(errp, "pcap is not supported in this build");
    return -1;
}
