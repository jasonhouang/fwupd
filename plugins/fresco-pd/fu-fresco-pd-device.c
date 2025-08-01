/*
 * Copyright 2020 Fresco Logic
 * Copyright 2020 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-fresco-pd-common.h"
#include "fu-fresco-pd-device.h"
#include "fu-fresco-pd-firmware.h"

struct _FuFrescoPdDevice {
	FuUsbDevice parent_instance;
	guint8 customer_id;
};

G_DEFINE_TYPE(FuFrescoPdDevice, fu_fresco_pd_device, FU_TYPE_USB_DEVICE)

static void
fu_fresco_pd_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuFrescoPdDevice *self = FU_FRESCO_PD_DEVICE(device);
	fwupd_codec_string_append_int(str, idt, "CustomerID", self->customer_id);
}

static gboolean
fu_fresco_pd_device_transfer_read(FuFrescoPdDevice *self,
				  guint16 offset,
				  guint8 *buf,
				  guint16 bufsz,
				  GError **error)
{
	gsize actual_length = 0;

	g_return_val_if_fail(buf != NULL, FALSE);
	g_return_val_if_fail(bufsz != 0, FALSE);

	/* to device */
	fu_dump_raw(G_LOG_DOMAIN, "read", buf, bufsz);
	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_DEVICE_TO_HOST,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    0x40,
					    0x0,
					    offset,
					    buf,
					    bufsz,
					    &actual_length,
					    5000,
					    NULL,
					    error)) {
		g_prefix_error(error, "failed to read from offset 0x%x: ", offset);
		fwupd_error_convert(error);
		return FALSE;
	}
	if (bufsz != actual_length) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "read 0x%x bytes of 0x%x",
			    (guint)actual_length,
			    bufsz);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_fresco_pd_device_transfer_write(FuFrescoPdDevice *self,
				   guint16 offset,
				   guint8 *buf,
				   guint16 bufsz,
				   GError **error)
{
	gsize actual_length = 0;

	g_return_val_if_fail(buf != NULL, FALSE);
	g_return_val_if_fail(bufsz != 0, FALSE);

	/* to device */
	fu_dump_raw(G_LOG_DOMAIN, "write", buf, bufsz);
	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    0x41,
					    0x0,
					    offset,
					    buf,
					    bufsz,
					    &actual_length,
					    5000,
					    NULL,
					    error)) {
		g_prefix_error(error, "failed to write offset 0x%x: ", offset);
		return FALSE;
	}
	if (bufsz != actual_length) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "wrote 0x%x bytes of 0x%x",
			    (guint)actual_length,
			    bufsz);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_fresco_pd_device_read_byte(FuFrescoPdDevice *self, guint16 offset, guint8 *buf, GError **error)
{
	return fu_fresco_pd_device_transfer_read(self, offset, buf, 1, error);
}

static gboolean
fu_fresco_pd_device_write_byte(FuFrescoPdDevice *self, guint16 offset, guint8 buf, GError **error)
{
	return fu_fresco_pd_device_transfer_write(self, offset, &buf, 1, error);
}

static gboolean
fu_fresco_pd_device_set_byte(FuFrescoPdDevice *self, guint16 offset, guint8 val, GError **error)
{
	guint8 buf = 0x0;
	if (!fu_fresco_pd_device_read_byte(self, offset, &buf, error))
		return FALSE;
	if (buf == val)
		return TRUE;
	return fu_fresco_pd_device_write_byte(self, offset, val, error);
}

static gboolean
fu_fresco_pd_device_and_byte(FuFrescoPdDevice *self, guint16 offset, guint8 val, GError **error)
{
	guint8 buf = 0xff;
	if (!fu_fresco_pd_device_read_byte(self, offset, &buf, error))
		return FALSE;
	buf &= val;
	return fu_fresco_pd_device_write_byte(self, offset, buf, error);
}

static gboolean
fu_fresco_pd_device_or_byte(FuFrescoPdDevice *self, guint16 offset, guint8 val, GError **error)
{
	guint8 buf;
	if (!fu_fresco_pd_device_read_byte(self, offset, &buf, error))
		return FALSE;
	buf |= val;
	return fu_fresco_pd_device_write_byte(self, offset, buf, error);
}

static gboolean
fu_fresco_pd_device_setup(FuDevice *device, GError **error)
{
	FuFrescoPdDevice *self = FU_FRESCO_PD_DEVICE(device);
	guint8 ver[4] = {0x0};
	g_autofree gchar *version = NULL;

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_fresco_pd_device_parent_class)->setup(device, error))
		return FALSE;

	/* read existing device version */
	for (guint i = 0; i < 4; i++) {
		if (!fu_fresco_pd_device_transfer_read(self, 0x3000 + i, &ver[i], 1, error)) {
			g_prefix_error(error, "failed to read device version [%u]: ", i);
			return FALSE;
		}
	}
	version = fu_fresco_pd_version_from_buf(ver);
	fu_device_set_version(FU_DEVICE(self), version);

	/* get customer ID */
	self->customer_id = ver[1];

	/* add extra instance ID */
	fu_device_add_instance_u8(device, "CID", self->customer_id);
	return fu_device_build_instance_id(device, error, "USB", "VID", "PID", "CID", NULL);
}

static FuFirmware *
fu_fresco_pd_device_prepare_firmware(FuDevice *device,
				     GInputStream *stream,
				     FuProgress *progress,
				     FuFirmwareParseFlags flags,
				     GError **error)
{
	FuFrescoPdDevice *self = FU_FRESCO_PD_DEVICE(device);
	guint8 customer_id;
	g_autoptr(FuFirmware) firmware = fu_fresco_pd_firmware_new();

	/* check firmware is suitable */
	if (!fu_firmware_parse_stream(firmware, stream, 0x0, flags, error))
		return NULL;
	customer_id = fu_fresco_pd_firmware_get_customer_id(FU_FRESCO_PD_FIRMWARE(firmware));
	if (customer_id != self->customer_id) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_FILE,
			    "device is incompatible with firmware x.%u.x.x",
			    customer_id);
		return NULL;
	}
	return g_steal_pointer(&firmware);
}

static gboolean
fu_fresco_pd_device_panther_reset_device(FuFrescoPdDevice *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;

	g_debug("resetting target device");
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);

	/* ignore when the device reset before completing the transaction */
	if (!fu_fresco_pd_device_or_byte(self, 0xA003, 1 << 3, &error_local)) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_INTERNAL)) {
			g_debug("ignoring %s", error_local->message);
			return TRUE;
		}
		g_propagate_prefixed_error(error,
					   g_steal_pointer(&error_local),
					   "failed to reset device: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_fresco_pd_device_write_firmware(FuDevice *device,
				   FuFirmware *firmware,
				   FuProgress *progress,
				   FwupdInstallFlags flags,
				   GError **error)
{
	FuFrescoPdDevice *self = FU_FRESCO_PD_DEVICE(device);
	const guint8 *buf;
	gsize bufsz = 0x0;
	guint16 begin_addr = 0x6420;
	guint8 config[3] = {0x0};
	guint8 start_symbols[2] = {0x0};
	g_autoptr(GBytes) fw = NULL;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 2, "enable-mtp-write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 50, "copy-mmio");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_VERIFY, 46, "customize");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 2, "boot");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 2, NULL);

	/* get default blob, which we know is already bigger than FirmwareMin */
	fw = fu_firmware_get_bytes(firmware, error);
	if (fw == NULL)
		return FALSE;
	buf = g_bytes_get_data(fw, &bufsz);

	/* get start symbols, and be slightly paranoid */
	if (!fu_memcpy_safe(start_symbols,
			    sizeof(start_symbols),
			    0x0, /* dst */
			    buf,
			    bufsz,
			    0x4000, /* src */
			    sizeof(start_symbols),
			    error))
		return FALSE;

	/* 0xA001<bit 2> = b'0
	 * 0x6C00<bit 1> = b'0
	 * 0x6C04 = 0x08 */
	g_debug("disable MCU, and enable mtp write");
	if (!fu_fresco_pd_device_and_byte(self, 0xa001, ~(1 << 2), error)) {
		g_prefix_error(error, "failed to disable MCU bit 2: ");
		return FALSE;
	}
	if (!fu_fresco_pd_device_and_byte(self, 0x6c00, ~(1 << 1), error)) {
		g_prefix_error(error, "failed to disable MCU bit 1: ");
		return FALSE;
	}
	if (!fu_fresco_pd_device_write_byte(self, 0x6c04, 0x08, error)) {
		g_prefix_error(error, "failed to disable MCU: ");
		return FALSE;
	}

	/* fill safe code in the boot code */
	for (guint16 i = 0; i < 0x400; i += 3) {
		for (guint j = 0; j < 3; j++) {
			if (!fu_fresco_pd_device_read_byte(self,
							   begin_addr + i + j,
							   &config[j],
							   error)) {
				g_prefix_error(error, "failed to read config byte %u: ", j);
				return FALSE;
			}
		}
		if (config[0] == start_symbols[0] && config[1] == start_symbols[1]) {
			begin_addr = 0x6420 + i;
			break;
		}
		if (config[0] == 0 && config[1] == 0 && config[2] == 0)
			break;
	}
	g_debug("begin_addr: 0x%04x", begin_addr);
	for (guint i = begin_addr + 3; i < (guint)begin_addr + 0x400; i += 3) {
		for (guint j = 0; j < 3; j++) {
			if (!fu_fresco_pd_device_read_byte(self, i + j, &config[j], error)) {
				g_prefix_error(error, "failed to read config byte %u: ", j);
				return FALSE;
			}
		}
		if (config[0] == 0x74 && config[1] == 0x06 && config[2] != 0x22) {
			if (!fu_fresco_pd_device_write_byte(self, i + 2, 0x22, error))
				return FALSE;
		} else if (config[0] == 0x6c && config[1] == 0x00 && config[2] != 0x01) {
			if (!fu_fresco_pd_device_write_byte(self, i + 2, 0x01, error))
				return FALSE;
		} else if (config[0] == 0x00 && config[1] == 0x00 && config[2] != 0x00)
			break;
	}
	fu_progress_step_done(progress);

	/* copy buf offset [0 - 0x3FFFF] to mmio address [0x2000 - 0x5FFF] */
	g_debug("fill firmware body");
	for (guint16 byte_index = 0; byte_index < 0x4000; byte_index++) {
		if (!fu_fresco_pd_device_set_byte(self,
						  byte_index + 0x2000,
						  buf[byte_index],
						  error))
			return FALSE;
		fu_progress_set_percentage_full(fu_progress_get_child(progress),
						(gsize)byte_index + 1,
						0x4000);
	}
	fu_progress_step_done(progress);

	/* write file buf 0x4200 ~ 0x4205, 6 bytes to internal address 0x6600 ~ 0x6605
	 * write file buf 0x4210 ~ 0x4215, 6 bytes to internal address 0x6610 ~ 0x6615
	 * write file buf 0x4220 ~ 0x4225, 6 bytes to internal address 0x6620 ~ 0x6625
	 * write file buf 0x4230, 1 byte, to internal address 0x6630 */
	g_debug("update customize data");
	for (guint16 byte_index = 0; byte_index < 6; byte_index++) {
		if (!fu_fresco_pd_device_set_byte(self,
						  0x6600 + byte_index,
						  buf[0x4200 + byte_index],
						  error))
			return FALSE;
		if (!fu_fresco_pd_device_set_byte(self,
						  0x6610 + byte_index,
						  buf[0x4210 + byte_index],
						  error))
			return FALSE;
		if (!fu_fresco_pd_device_set_byte(self,
						  0x6620 + byte_index,
						  buf[0x4220 + byte_index],
						  error))
			return FALSE;
	}
	if (!fu_fresco_pd_device_set_byte(self, 0x6630, buf[0x4230], error))
		return FALSE;
	fu_progress_step_done(progress);

	/* overwrite firmware file's boot code area (0x4020 ~ 0x41ff) to the area on the device
	 * marked by begin_addr example: if the begin_addr = 0x6420, then copy file buf [0x4020 ~
	 * 0x41ff] to device offset[0x6420 ~ 0x65ff] */
	g_debug("write boot configuration area");
	for (guint16 byte_index = 0; byte_index < 0x1e0; byte_index += 3) {
		if (!fu_fresco_pd_device_set_byte(self,
						  begin_addr + byte_index + 0,
						  buf[0x4020 + byte_index],
						  error))
			return FALSE;
		if (!fu_fresco_pd_device_set_byte(self,
						  begin_addr + byte_index + 1,
						  buf[0x4021 + byte_index],
						  error))
			return FALSE;
		if (!fu_fresco_pd_device_set_byte(self,
						  begin_addr + byte_index + 2,
						  buf[0x4022 + byte_index],
						  error))
			return FALSE;
	}
	fu_progress_step_done(progress);

	/* reset the device */
	if (!fu_fresco_pd_device_panther_reset_device(self, error))
		return FALSE;
	fu_progress_step_done(progress);

	/* success */
	return TRUE;
}

static void
fu_fresco_pd_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-fw");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 100, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 0, "reload");
}

static void
fu_fresco_pd_device_init(FuFrescoPdDevice *self)
{
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_USB_HUB);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
	fu_device_add_protocol(FU_DEVICE(self), "com.frescologic.pd");
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_QUAD);
	fu_device_set_install_duration(FU_DEVICE(self), 15);
	fu_device_set_remove_delay(FU_DEVICE(self), 20000);
	fu_device_set_firmware_size(FU_DEVICE(self), 0x4400);
}

static void
fu_fresco_pd_device_class_init(FuFrescoPdDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->to_string = fu_fresco_pd_device_to_string;
	device_class->setup = fu_fresco_pd_device_setup;
	device_class->write_firmware = fu_fresco_pd_device_write_firmware;
	device_class->prepare_firmware = fu_fresco_pd_device_prepare_firmware;
	device_class->set_progress = fu_fresco_pd_device_set_progress;
}
