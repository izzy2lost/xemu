/*
 * OS-specific Helpers for iPhone/iPad embedded builds
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <glib.h>
#include "xemu-os-utils.h"

const char *xemu_get_os_info(void)
{
    static const char *os_info = NULL;

    if (os_info == NULL) {
        UIDevice *device = [UIDevice currentDevice];
        NSString *description = [NSString stringWithFormat:@"%@ %@",
                                 device.systemName ?: @"iOS",
                                 device.systemVersion ?: @""];
        os_info = g_strdup(description.UTF8String ?: "iOS");
    }

    return os_info;
}
