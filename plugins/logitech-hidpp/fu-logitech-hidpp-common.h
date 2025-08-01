/*
 * Copyright 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#define FU_LOGITECH_HIDPP_DEVICE_VID 0x046d

#define FU_LOGITECH_HIDPP_DEVICE_PID_RUNTIME		    0xC52B
#define FU_LOGITECH_HIDPP_DEVICE_PID_BOOTLOADER_NORDIC	    0xAAAA
#define FU_LOGITECH_HIDPP_DEVICE_PID_BOOTLOADER_NORDIC_PICO 0xAAAE
#define FU_LOGITECH_HIDPP_DEVICE_PID_BOOTLOADER_TEXAS	    0xAAAC
#define FU_LOGITECH_HIDPP_DEVICE_PID_BOOTLOADER_TEXAS_PICO  0xAAAD
#define FU_LOGITECH_HIDPP_DEVICE_PID_BOOTLOADER_BOLT	    0xAB07

/* Signed firmware are very long to verify on the device */
#define FU_LOGITECH_HIDPP_DEVICE_TIMEOUT_MS 30000

/* Polling intervals (ms) */
#define FU_LOGITECH_HIDPP_DEVICE_POLLING_INTERVAL	    30000
#define FU_LOGITECH_HIDPP_RECEIVER_RUNTIME_POLLING_INTERVAL 5000

#define FU_LOGITECH_HIDPP_VERSION_BLE 0xFE

guint8
fu_logitech_hidpp_buffer_read_uint8(const gchar *str);
guint16
fu_logitech_hidpp_buffer_read_uint16(const gchar *str);

gchar *
fu_logitech_hidpp_format_version(const gchar *name, guint8 major, guint8 minor, guint16 build);
