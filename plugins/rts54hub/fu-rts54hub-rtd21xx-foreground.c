/*
 * Copyright 2021 Realtek Corporation
 * Copyright 2021 Ricky Wu <ricky_wu@realtek.com> <spring1527@gmail.com>
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-rts54hub-device.h"
#include "fu-rts54hub-rtd21xx-foreground.h"

struct _FuRts54hubRtd21xxForeground {
	FuRts54hubRtd21xxDevice parent_instance;
};

G_DEFINE_TYPE(FuRts54hubRtd21xxForeground,
	      fu_rts54hub_rtd21xx_foreground,
	      FU_TYPE_RTS54HUB_RTD21XX_DEVICE)

#define ISP_DATA_BLOCKSIZE 256
#define ISP_PACKET_SIZE	   257

typedef enum {
	ISP_CMD_ENTER_FW_UPDATE = 0x01,
	ISP_CMD_GET_PROJECT_ID_ADDR = 0x02,
	ISP_CMD_SYNC_IDENTIFY_CODE = 0x03,
	ISP_CMD_GET_FW_INFO = 0x04,
	ISP_CMD_FW_UPDATE_START = 0x05,
	ISP_CMD_FW_UPDATE_ISP_DONE = 0x06,
	ISP_CMD_FW_UPDATE_RESET = 0x07,
	ISP_CMD_FW_UPDATE_EXIT = 0x08,
} IspCmd;

static gboolean
fu_rts54hub_rtd21xx_foreground_ensure_version_unlocked(FuRts54hubRtd21xxForeground *self,
						       GError **error)
{
	guint8 buf_rep[7] = {0x00};
	guint8 buf_req[] = {ISP_CMD_GET_FW_INFO};
	g_autofree gchar *version = NULL;

	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  buf_req,
						  sizeof(buf_req),
						  error)) {
		g_prefix_error(error, "failed to get version number: ");
		return FALSE;
	}

	/* wait for device ready */
	fu_device_sleep(FU_DEVICE(self), 300); /* ms */
	if (!fu_rts54hub_rtd21xx_device_i2c_read(FU_RTS54HUB_RTD21XX_DEVICE(self),
						 UC_ISP_TARGET_ADDR,
						 0x00,
						 buf_rep,
						 sizeof(buf_rep),
						 error)) {
		g_prefix_error(error, "failed to get version number: ");
		return FALSE;
	}
	/* set version */
	version = g_strdup_printf("%u.%u", buf_rep[1], buf_rep[2]);
	fu_device_set_version(FU_DEVICE(self), version);
	return TRUE;
}

static gboolean
fu_rts54hub_rtd21xx_foreground_detach_raw(FuRts54hubRtd21xxForeground *self, GError **error)
{
	guint8 buf = 0x03;
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  0x6A,
						  0x31,
						  &buf,
						  sizeof(buf),
						  error)) {
		g_prefix_error(error, "failed to detach: ");
		return FALSE;
	}
	/* wait for device ready */
	fu_device_sleep(FU_DEVICE(self), 300); /* ms */
	return TRUE;
}

static gboolean
fu_rts54hub_rtd21xx_foreground_detach_cb(FuDevice *device, gpointer user_data, GError **error)
{
	FuRts54hubRtd21xxForeground *self = FU_RTS54HUB_RTD21XX_FOREGROUND(device);
	guint8 status = 0xfe;

	if (!fu_rts54hub_rtd21xx_foreground_detach_raw(self, error))
		return FALSE;
	if (!fu_rts54hub_rtd21xx_device_read_status_raw(FU_RTS54HUB_RTD21XX_DEVICE(self),
							&status,
							error))
		return FALSE;
	if (status != ISP_STATUS_IDLE_SUCCESS) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "detach status was 0x%02x",
			    status);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_rts54hub_rtd21xx_foreground_detach(FuDevice *device, FuProgress *progress, GError **error)
{
	FuRts54hubDevice *parent = FU_RTS54HUB_DEVICE(fu_device_get_parent(device));
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* open device */
	locker = fu_device_locker_new(parent, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_retry(device, fu_rts54hub_rtd21xx_foreground_detach_cb, 100, NULL, error);
}

static gboolean
fu_rts54hub_rtd21xx_foreground_attach(FuDevice *device, FuProgress *progress, GError **error)
{
	FuRts54hubDevice *parent = FU_RTS54HUB_DEVICE(fu_device_get_parent(device));
	FuRts54hubRtd21xxForeground *self = FU_RTS54HUB_RTD21XX_FOREGROUND(device);
	guint8 buf[] = {ISP_CMD_FW_UPDATE_RESET};
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* open device */
	locker = fu_device_locker_new(parent, error);
	if (locker == NULL)
		return FALSE;

	/* exit fw mode */
	if (!fu_rts54hub_rtd21xx_device_read_status(FU_RTS54HUB_RTD21XX_DEVICE(self), NULL, error))
		return FALSE;
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  buf,
						  sizeof(buf),
						  error)) {
		g_prefix_error(error, "failed to ISP_CMD_FW_UPDATE_RESET: ");
		return FALSE;
	}

	/* the device needs some time to restart with the new firmware before
	 * it can be queried again */
	fu_device_sleep_full(device, 60000, progress); /* ms */

	/* success */
	return TRUE;
}

static gboolean
fu_rts54hub_rtd21xx_foreground_exit(FuDevice *device, GError **error)
{
	FuRts54hubDevice *parent = FU_RTS54HUB_DEVICE(fu_device_get_parent(device));
	FuRts54hubRtd21xxForeground *self = FU_RTS54HUB_RTD21XX_FOREGROUND(device);
	guint8 buf[] = {ISP_CMD_FW_UPDATE_EXIT};
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* open device */
	locker = fu_device_locker_new(parent, error);
	if (locker == NULL)
		return FALSE;

	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  buf,
						  sizeof(buf),
						  error)) {
		g_prefix_error(error, "failed to ISP_CMD_FW_UPDATE_EXIT: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_rts54hub_rtd21xx_foreground_setup(FuDevice *device, GError **error)
{
	FuRts54hubRtd21xxForeground *self = FU_RTS54HUB_RTD21XX_FOREGROUND(device);
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* get version */
	locker = fu_device_locker_new_full(device,
					   (FuDeviceLockerFunc)fu_device_detach,
					   (FuDeviceLockerFunc)fu_rts54hub_rtd21xx_foreground_exit,
					   error);
	if (locker == NULL)
		return FALSE;
	if (!fu_rts54hub_rtd21xx_foreground_ensure_version_unlocked(self, error))
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
fu_rts54hub_rtd21xx_foreground_reload(FuDevice *device, GError **error)
{
	FuRts54hubDevice *parent = FU_RTS54HUB_DEVICE(fu_device_get_parent(device));
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* open parent device */
	locker = fu_device_locker_new(parent, error);
	if (locker == NULL)
		return FALSE;
	return fu_rts54hub_rtd21xx_foreground_setup(device, error);
}

static gboolean
fu_rts54hub_rtd21xx_foreground_write_firmware(FuDevice *device,
					      FuFirmware *firmware,
					      FuProgress *progress,
					      FwupdInstallFlags flags,
					      GError **error)
{
	FuRts54hubRtd21xxForeground *self = FU_RTS54HUB_RTD21XX_FOREGROUND(device);
	guint32 project_addr;
	guint8 project_id_count;
	guint8 read_buf[10] = {0x0};
	guint8 write_buf[ISP_PACKET_SIZE] = {0x0};
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GInputStream) stream = NULL;
	g_autoptr(FuChunkArray) chunks = NULL;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 5, "setup");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 90, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 5, "finish");

	/* open device */
	locker = fu_device_locker_new(self, error);
	if (locker == NULL)
		return FALSE;

	/* simple image */
	stream = fu_firmware_get_stream(firmware, error);
	if (stream == NULL)
		return FALSE;

	/* enable ISP high priority */
	write_buf[0] = ISP_CMD_ENTER_FW_UPDATE;
	write_buf[1] = 0x01;
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  write_buf,
						  2,
						  error)) {
		g_prefix_error(error, "failed to enable ISP: ");
		return FALSE;
	}
	if (!fu_rts54hub_rtd21xx_device_read_status(FU_RTS54HUB_RTD21XX_DEVICE(self), NULL, error))
		return FALSE;

	/* get project ID address */
	write_buf[0] = ISP_CMD_GET_PROJECT_ID_ADDR;
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  write_buf,
						  1,
						  error)) {
		g_prefix_error(error, "failed to get project ID address: ");
		return FALSE;
	}

	/* read back 6 bytes data */
	fu_device_sleep(FU_DEVICE(self), I2C_DELAY_AFTER_SEND * 40);
	if (!fu_rts54hub_rtd21xx_device_i2c_read(FU_RTS54HUB_RTD21XX_DEVICE(self),
						 UC_ISP_TARGET_ADDR,
						 UC_FOREGROUND_STATUS,
						 read_buf,
						 6,
						 error)) {
		g_prefix_error(error, "failed to read project ID: ");
		return FALSE;
	}
	if (read_buf[0] != ISP_STATUS_IDLE_SUCCESS) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "failed project ID with error 0x%02x: ",
			    read_buf[0]);
		return FALSE;
	}

	/* verify project ID */
	project_addr = fu_memread_uint32(read_buf + 1, G_BIG_ENDIAN);
	project_id_count = read_buf[5];
	write_buf[0] = ISP_CMD_SYNC_IDENTIFY_CODE;
	if (!fu_input_stream_read_safe(stream,
				       write_buf,
				       sizeof(write_buf),
				       0x1,	     /* dst */
				       project_addr, /* src */
				       project_id_count,
				       error)) {
		g_prefix_error(error, "failed to write project ID from 0x%04x: ", project_addr);
		return FALSE;
	}
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  write_buf,
						  project_id_count + 1,
						  error)) {
		g_prefix_error(error, "failed to send fw update start cmd: ");
		return FALSE;
	}
	if (!fu_rts54hub_rtd21xx_device_read_status(FU_RTS54HUB_RTD21XX_DEVICE(self), NULL, error))
		return FALSE;

	/* foreground FW update start command */
	write_buf[0] = ISP_CMD_FW_UPDATE_START;
	fu_memwrite_uint16(write_buf + 1, ISP_DATA_BLOCKSIZE, G_BIG_ENDIAN);
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  write_buf,
						  3,
						  error)) {
		g_prefix_error(error, "failed to send fw update start cmd: ");
		return FALSE;
	}
	fu_progress_step_done(progress);

	/* send data */
	chunks = fu_chunk_array_new_from_stream(stream,
						FU_CHUNK_ADDR_OFFSET_NONE,
						FU_CHUNK_PAGESZ_NONE,
						ISP_DATA_BLOCKSIZE,
						error);
	if (chunks == NULL)
		return FALSE;
	for (guint i = 0; i < fu_chunk_array_length(chunks); i++) {
		g_autoptr(FuChunk) chk = NULL;

		/* prepare chunk */
		chk = fu_chunk_array_index(chunks, i, error);
		if (chk == NULL)
			return FALSE;
		if (!fu_rts54hub_rtd21xx_device_read_status(FU_RTS54HUB_RTD21XX_DEVICE(self),
							    NULL,
							    error))
			return FALSE;
		if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
							  UC_ISP_TARGET_ADDR,
							  UC_FOREGROUND_ISP_DATA_OPCODE,
							  fu_chunk_get_data(chk),
							  fu_chunk_get_data_sz(chk),
							  error)) {
			g_prefix_error(error,
				       "failed to write @0x%04x: ",
				       (guint)fu_chunk_get_address(chk));
			return FALSE;
		}

		/* update progress */
		fu_progress_set_percentage_full(fu_progress_get_child(progress),
						(gsize)i + 1,
						(gsize)fu_chunk_array_length(chunks));
	}
	fu_progress_step_done(progress);

	/* update finish command */
	if (!fu_rts54hub_rtd21xx_device_read_status(FU_RTS54HUB_RTD21XX_DEVICE(self), NULL, error))
		return FALSE;
	write_buf[0] = ISP_CMD_FW_UPDATE_ISP_DONE;
	if (!fu_rts54hub_rtd21xx_device_i2c_write(FU_RTS54HUB_RTD21XX_DEVICE(self),
						  UC_ISP_TARGET_ADDR,
						  UC_FOREGROUND_OPCODE,
						  write_buf,
						  1,
						  error)) {
		g_prefix_error(error, "failed update finish cmd: ");
		return FALSE;
	}
	fu_progress_step_done(progress);

	/* success */
	return TRUE;
}

static void
fu_rts54hub_rtd21xx_foreground_init(FuRts54hubRtd21xxForeground *self)
{
}

static void
fu_rts54hub_rtd21xx_foreground_class_init(FuRts54hubRtd21xxForegroundClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->setup = fu_rts54hub_rtd21xx_foreground_setup;
	device_class->reload = fu_rts54hub_rtd21xx_foreground_reload;
	device_class->attach = fu_rts54hub_rtd21xx_foreground_attach;
	device_class->detach = fu_rts54hub_rtd21xx_foreground_detach;
	device_class->write_firmware = fu_rts54hub_rtd21xx_foreground_write_firmware;
}
