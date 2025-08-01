/*
 * Copyright 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>

#include "fu-logitech-hidpp-bootloader.h"
#include "fu-logitech-hidpp-common.h"
#include "fu-logitech-hidpp-hidpp.h"
#include "fu-logitech-hidpp-struct.h"

typedef struct {
	guint16 flash_addr_lo;
	guint16 flash_addr_hi;
	guint16 flash_blocksize;
} FuLogitechHidppBootloaderPrivate;

#define FU_LOGITECH_HIDPP_DEVICE_EP1 0x81
#define FU_LOGITECH_HIDPP_DEVICE_EP3 0x83

G_DEFINE_TYPE_WITH_PRIVATE(FuLogitechHidppBootloader,
			   fu_logitech_hidpp_bootloader,
			   FU_TYPE_HID_DEVICE)

#define GET_PRIVATE(o) (fu_logitech_hidpp_bootloader_get_instance_private(o))

static void
fu_logitech_hidpp_bootloader_to_string(FuDevice *device, guint idt, GString *str)
{
	FuLogitechHidppBootloader *self = FU_LOGITECH_HIDPP_BOOTLOADER(device);
	FuLogitechHidppBootloaderPrivate *priv = GET_PRIVATE(self);
	fwupd_codec_string_append_hex(str, idt, "FlashAddrHigh", priv->flash_addr_hi);
	fwupd_codec_string_append_hex(str, idt, "FlashAddrLow", priv->flash_addr_lo);
	fwupd_codec_string_append_hex(str, idt, "FlashBlockSize", priv->flash_blocksize);
}

FuLogitechHidppBootloaderRequest *
fu_logitech_hidpp_bootloader_request_new(void)
{
	FuLogitechHidppBootloaderRequest *req = g_new0(FuLogitechHidppBootloaderRequest, 1);
	return req;
}

GPtrArray *
fu_logitech_hidpp_bootloader_parse_requests(FuLogitechHidppBootloader *self,
					    GBytes *fw,
					    GError **error)
{
	const gchar *tmp;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GPtrArray) reqs = NULL;
	guint32 last_addr = 0;

	reqs = g_ptr_array_new_with_free_func(g_free);
	tmp = g_bytes_get_data(fw, NULL);
	lines = g_strsplit_set(tmp, "\n\r", -1);
	for (guint i = 0; lines[i] != NULL; i++) {
		g_autoptr(FuLogitechHidppBootloaderRequest) payload = NULL;
		guint8 rec_type = 0x00;
		guint16 offset = 0x0000;
		guint16 addr = 0x0;
		gboolean exit = FALSE;
		gsize linesz = strlen(lines[i]);

		/* skip empty lines */
		tmp = lines[i];
		if (linesz < 5)
			continue;

		payload = fu_logitech_hidpp_bootloader_request_new();
		payload->len = fu_logitech_hidpp_buffer_read_uint8(tmp + 0x01);
		if (payload->len > 28) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "firmware data invalid: too large %u bytes",
				    payload->len);
			return NULL;
		}
		if (!fu_firmware_strparse_uint16_safe(tmp, linesz, 0x03, &addr, error))
			return NULL;
		payload->addr = addr;
		payload->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_RAM_BUFFER;

		rec_type = fu_logitech_hidpp_buffer_read_uint8(tmp + 0x07);

		switch (rec_type) {
		case 0x00: /* data */
			break;
		case 0x01: /* EOF */
			exit = TRUE;
			break;
		case 0x03: /* start segment address */
			/* this is used to specify the start address,
			it is doesn't matter in this context so we can
			safely ignore it */
			continue;
		case 0x04: /* extended linear address */
			if (!fu_firmware_strparse_uint16_safe(tmp, linesz, 0x09, &offset, error))
				return NULL;
			if (offset != 0x0000) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_DATA,
					    "extended linear addresses with offset different from "
					    "0 are not supported");
				return NULL;
			}
			continue;
		case 0x05: /* start linear address */
			/* this is used to specify the start address,
			it is doesn't matter in this context so we can
			safely ignore it */
			continue;
		case 0xFD: /* custom - vendor */
			/* record type of 0xFD indicates signature data */
			payload->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_SIGNATURE;
			break;
		default:
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "intel hex file record type %02x not supported",
				    rec_type);
			return NULL;
		}

		if (exit)
			break;

		/* read the data, but skip the checksum byte */
		for (guint j = 0; j < payload->len; j++) {
			const gchar *ptr = tmp + 0x09 + (j * 2);
			if (ptr[0] == '\0') {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_DATA,
					    "firmware data invalid: expected %u bytes",
					    payload->len);
				return NULL;
			}
			payload->data[j] = fu_logitech_hidpp_buffer_read_uint8(ptr);
		}

		/* no need to bound check signature addresses */
		if (payload->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_SIGNATURE) {
			g_ptr_array_add(reqs, g_steal_pointer(&payload));
			continue;
		}

		/* skip the bootloader */
		if (payload->addr > fu_logitech_hidpp_bootloader_get_addr_hi(self)) {
			g_debug("skipping write @ %04x", payload->addr);
			continue;
		}

		/* skip the header */
		if (payload->addr < fu_logitech_hidpp_bootloader_get_addr_lo(self)) {
			g_debug("skipping write @ %04x", payload->addr);
			continue;
		}

		/* make sure firmware addresses only go up */
		if (payload->addr < last_addr) {
			g_debug("skipping write @ %04x", payload->addr);
			continue;
		}
		last_addr = payload->addr;

		/* pending */
		g_ptr_array_add(reqs, g_steal_pointer(&payload));
	}
	if (reqs->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "firmware data invalid: no payloads found");
		return NULL;
	}
	return g_steal_pointer(&reqs);
}

guint16
fu_logitech_hidpp_bootloader_get_addr_lo(FuLogitechHidppBootloader *self)
{
	FuLogitechHidppBootloaderPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_LOGITECH_HIDPP_BOOTLOADER(self), 0x0000);
	return priv->flash_addr_lo;
}

guint16
fu_logitech_hidpp_bootloader_get_addr_hi(FuLogitechHidppBootloader *self)
{
	FuLogitechHidppBootloaderPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_LOGITECH_HIDPP_BOOTLOADER(self), 0x0000);
	return priv->flash_addr_hi;
}

guint16
fu_logitech_hidpp_bootloader_get_blocksize(FuLogitechHidppBootloader *self)
{
	FuLogitechHidppBootloaderPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_LOGITECH_HIDPP_BOOTLOADER(self), 0x0000);
	return priv->flash_blocksize;
}

static gboolean
fu_logitech_hidpp_bootloader_attach(FuDevice *device, FuProgress *progress, GError **error)
{
	FuLogitechHidppBootloader *self = FU_LOGITECH_HIDPP_BOOTLOADER(device);
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_REBOOT;
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to attach back to runtime: ");
		return FALSE;
	}
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_logitech_hidpp_bootloader_set_bl_version(FuLogitechHidppBootloader *self, GError **error)
{
	guint16 build;
	guint8 major;
	guint8 minor;
	g_autofree gchar *version = NULL;
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();

	/* call into hardware */
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_GET_BL_VERSION;
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to get firmware version: ");
		return FALSE;
	}

	/* BOTxx.yy_Bzzzz
	 * 012345678901234 */
	build = (guint16)fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 10) << 8;
	build += fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 12);
	major = fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 3);
	minor = fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 6);
	version = fu_logitech_hidpp_format_version("BOT", major, minor, build);
	if (version == NULL) {
		g_prefix_error(error, "failed to format firmware version: ");
		return FALSE;
	}
	fu_device_set_version_bootloader(FU_DEVICE(self), version);

	if ((major == 0x01 && minor >= 0x04) || (major == 0x03 && minor >= 0x02)) {
		fu_device_add_private_flag(FU_DEVICE(self),
					   FU_LOGITECH_HIDPP_BOOTLOADER_FLAG_IS_SIGNED);
		fu_device_add_protocol(FU_DEVICE(self), "com.logitech.unifyingsigned");
	} else {
		fu_device_add_protocol(FU_DEVICE(self), "com.logitech.unifying");
	}
	return TRUE;
}

static gboolean
fu_logitech_hidpp_bootloader_setup(FuDevice *device, GError **error)
{
	FuLogitechHidppBootloader *self = FU_LOGITECH_HIDPP_BOOTLOADER(device);
	FuLogitechHidppBootloaderPrivate *priv = GET_PRIVATE(self);
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_logitech_hidpp_bootloader_parent_class)->setup(device, error))
		return FALSE;

	/* get memory map */
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_GET_MEMINFO;
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to get meminfo: ");
		return FALSE;
	}
	if (req->len != 0x06) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "failed to get meminfo: invalid size %02x",
			    req->len);
		return FALSE;
	}

	/* parse values */
	priv->flash_addr_lo = fu_memread_uint16(req->data + 0, G_BIG_ENDIAN);
	priv->flash_addr_hi = fu_memread_uint16(req->data + 2, G_BIG_ENDIAN);
	priv->flash_blocksize = fu_memread_uint16(req->data + 4, G_BIG_ENDIAN);

	/* get bootloader version */
	return fu_logitech_hidpp_bootloader_set_bl_version(self, error);
}

gboolean
fu_logitech_hidpp_bootloader_request(FuLogitechHidppBootloader *self,
				     FuLogitechHidppBootloaderRequest *req,
				     GError **error)
{
	gsize actual_length = 0;
	guint8 buf_request[32] = {0};
	guint8 buf_response[32] = {0};

	/* build packet */
	buf_request[0x00] = req->cmd;
	buf_request[0x01] = req->addr >> 8;
	buf_request[0x02] = req->addr & 0xff;
	buf_request[0x03] = req->len;
	if (!fu_memcpy_safe(buf_request,
			    sizeof(buf_request),
			    0x04, /* dst */
			    req->data,
			    sizeof(req->data),
			    0x0, /* src */
			    sizeof(req->data),
			    error))
		return FALSE;

	/* send request */
	fu_dump_raw(G_LOG_DOMAIN, "host->device", buf_request, sizeof(buf_request));
	if (!fu_hid_device_set_report(FU_HID_DEVICE(self),
				      0x0,
				      buf_request,
				      sizeof(buf_request),
				      FU_LOGITECH_HIDPP_DEVICE_TIMEOUT_MS,
				      FU_HID_DEVICE_FLAG_NONE,
				      error)) {
		g_prefix_error(error, "failed to send data: ");
		return FALSE;
	}

	/* no response required when rebooting */
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_REBOOT) {
		g_autoptr(GError) error_ignore = NULL;
		if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
						      FU_LOGITECH_HIDPP_DEVICE_EP1,
						      buf_response,
						      sizeof(buf_response),
						      &actual_length,
						      FU_LOGITECH_HIDPP_DEVICE_TIMEOUT_MS,
						      NULL,
						      &error_ignore)) {
			g_debug("ignoring: %s", error_ignore->message);
		} else {
			fu_dump_raw(G_LOG_DOMAIN, "device->host", buf_response, actual_length);
		}
		return TRUE;
	}

	/* get response */
	if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
					      FU_LOGITECH_HIDPP_DEVICE_EP1,
					      buf_response,
					      sizeof(buf_response),
					      &actual_length,
					      FU_LOGITECH_HIDPP_DEVICE_TIMEOUT_MS,
					      NULL,
					      error)) {
		g_prefix_error(error, "failed to get data: ");
		return FALSE;
	}
	fu_dump_raw(G_LOG_DOMAIN, "device->host", buf_response, actual_length);

	/* parse response */
	if ((buf_response[0x00] & 0xf0) != req->cmd) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid command response of %02x, expected %02x",
			    buf_response[0x00],
			    req->cmd);
		return FALSE;
	}
	req->cmd = buf_response[0x00];
	req->addr = ((guint16)buf_response[0x01] << 8) + buf_response[0x02];
	req->len = buf_response[0x03];
	if (req->len > 28) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid data size of %02x",
			    req->len);
		return FALSE;
	}
	memset(req->data, 0x00, 28);
	if (req->len > 0)
		memcpy(req->data, buf_response + 0x04, req->len); /* nocheck:blocked */
	return TRUE;
}

static void
fu_logitech_hidpp_bootloader_replace(FuDevice *device, FuDevice *donor)
{
	fu_device_incorporate_flag(device, donor, FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);
	fu_device_incorporate_flag(device, donor, FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
}

static void
fu_logitech_hidpp_bootloader_init(FuLogitechHidppBootloader *self)
{
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_REPLUG_MATCH_GUID);
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_USB_RECEIVER);
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_set_name(FU_DEVICE(self), "Unifying Receiver");
	fu_device_set_summary(FU_DEVICE(self), "Miniaturised USB wireless receiver (bootloader)");
	fu_device_set_remove_delay(FU_DEVICE(self), FU_LOGITECH_HIDPP_DEVICE_TIMEOUT_MS);
	fu_device_register_private_flag(FU_DEVICE(self),
					FU_LOGITECH_HIDPP_BOOTLOADER_FLAG_IS_SIGNED);
	fu_usb_device_add_interface(FU_USB_DEVICE(self), 0x00);
}

static void
fu_logitech_hidpp_bootloader_class_init(FuLogitechHidppBootloaderClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->to_string = fu_logitech_hidpp_bootloader_to_string;
	device_class->attach = fu_logitech_hidpp_bootloader_attach;
	device_class->setup = fu_logitech_hidpp_bootloader_setup;
	device_class->replace = fu_logitech_hidpp_bootloader_replace;
}
