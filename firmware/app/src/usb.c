/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB device of the CB-91AI self-test: one composite device with two CDC-ACM
 * ports, the first for the shell and the logs, the second for the MCUmgr SMP
 * transport. Zephyr's own at-boot helper (CDC_ACM_SERIAL_INITIALIZE_AT_BOOT)
 * only registers the first CDC-ACM instance, hence this file.
 */

#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cb91ai_usb, LOG_LEVEL_INF);

static bool configured;

/* Set once the host has configured the device: the update path is proven to work */
bool cb91ai_usb_configured(void)
{
	return configured;
}

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (msg->type == USBD_MSG_CONFIGURATION) {
		configured = true;
		LOG_INF("USB configured by the host");
	} else if (msg->type == USBD_MSG_RESET || msg->type == USBD_MSG_VBUS_REMOVED) {
		configured = false;
	}
}

/* Zephyr test IDs: replace with the project's own VID/PID before any product use */
#define CB91AI_USB_VID 0x2fe3
#define CB91AI_USB_PID 0x0004
/* bMaxPower in 2 mA units: 100 mA, plenty for the board on USB */
#define CB91AI_USB_MAX_POWER 50

USBD_DEVICE_DEFINE(cb91ai_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CB91AI_USB_VID, CB91AI_USB_PID);

USBD_DESC_LANG_DEFINE(cb91ai_lang);
USBD_DESC_MANUFACTURER_DEFINE(cb91ai_mfr, "COBALT");
USBD_DESC_PRODUCT_DEFINE(cb91ai_product, "CB-91AI self-test");
USBD_DESC_SERIAL_NUMBER_DEFINE(cb91ai_sn);
USBD_DESC_CONFIG_DEFINE(cb91ai_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(cb91ai_fs_config, USB_SCD_SELF_POWERED, CB91AI_USB_MAX_POWER,
			  &cb91ai_fs_cfg_desc);

static int cb91ai_usb_init(void)
{
	struct usbd_desc_node *const descriptors[] = {
		&cb91ai_lang, &cb91ai_mfr, &cb91ai_product, &cb91ai_sn,
	};
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(descriptors); i++) {
		err = usbd_add_descriptor(&cb91ai_usbd, descriptors[i]);
		if (err) {
			LOG_ERR("USB descriptor %u failed (%d)", (unsigned int)i, err);
			return err;
		}
	}

	err = usbd_add_configuration(&cb91ai_usbd, USBD_SPEED_FS, &cb91ai_fs_config);
	if (err) {
		LOG_ERR("USB configuration failed (%d)", err);
		return err;
	}

	/* Every class instance found in the devicetree: cdc_acm_0 and cdc_acm_1 */
	err = usbd_register_all_classes(&cb91ai_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("USB class registration failed (%d)", err);
		return err;
	}

	/* Composite device with Interface Association Descriptors */
	usbd_device_set_code_triple(&cb91ai_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_self_powered(&cb91ai_usbd, true);

	err = usbd_msg_register_cb(&cb91ai_usbd, usb_msg_cb);
	if (err) {
		LOG_ERR("USB message callback failed (%d)", err);
		return err;
	}

	err = usbd_init(&cb91ai_usbd);
	if (err) {
		LOG_ERR("USB init failed (%d)", err);
		return err;
	}
	err = usbd_enable(&cb91ai_usbd);
	if (err) {
		LOG_ERR("USB enable failed (%d)", err);
		return err;
	}
	LOG_INF("USB device enabled: two CDC-ACM ports (shell and log, MCUmgr)");
	return 0;
}

SYS_INIT(cb91ai_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
