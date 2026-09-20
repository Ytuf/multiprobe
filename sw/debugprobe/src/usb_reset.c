/*
 * Raspberry Pi USB reset interface (vendor class, subclass 0, protocol 1).
 *
 * Minimal re-implementation of the pico-sdk's
 * src/rp2_common/pico_stdio_usb/reset_interface.c for firmware that drives
 * TinyUSB from its own descriptor table: linking pico_stdio_usb would take
 * ownership of the USB device and add a CDC of its own, which collides with
 * the hand-rolled descriptors in usb_descriptors.c.
 *
 * Without this interface picotool cannot see the board at all -
 * picoboot_open_device() classifies a non-BOOTSEL device as
 * dr_vidpid_stdio_usb only if it exposes an interface with
 * bInterfaceClass 0xff / RESET_INTERFACE_SUBCLASS / RESET_INTERFACE_PROTOCOL.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb_reset.h"

#include "pico/bootrom.h"
#include "hardware/watchdog.h"

static uint8_t reset_itf_num = USB_RESET_ITF_INVALID;

uint8_t usb_reset_itf_num(void)
{
	return reset_itf_num;
}

static void resetd_init(void)
{
}

static void resetd_reset(uint8_t __unused rhport)
{
	reset_itf_num = USB_RESET_ITF_INVALID;
}

static uint16_t resetd_open(uint8_t __unused rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
	/* Protocol 1 is what separates this from the four CMSIS-DAP interfaces,
	 * which are also TUSB_CLASS_VENDOR_SPECIFIC but subclass 0 / protocol 0
	 * and are claimed by _dap_edpt_driver (registered ahead of this one). */
	TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass &&
		  RESET_INTERFACE_SUBCLASS   == itf_desc->bInterfaceSubClass &&
		  RESET_INTERFACE_PROTOCOL   == itf_desc->bInterfaceProtocol, 0);

	uint16_t const drv_len = sizeof(tusb_desc_interface_t);
	TU_VERIFY(max_len >= drv_len, 0);

	reset_itf_num = itf_desc->bInterfaceNumber;
	return drv_len;
}

bool usb_reset_handle_request(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	/* Nothing to do at DATA / ACK stage; both requests are wLength == 0. */
	if (stage != CONTROL_STAGE_SETUP)
		return true;

	if (reset_itf_num == USB_RESET_ITF_INVALID || request->wIndex != reset_itf_num)
		return false;

	switch (request->bRequest) {
	case RESET_REQUEST_BOOTSEL: {
		/* wValue as picotool builds it: bits 0..6 disable_interface_mask,
		 * bit 7 activity LED active low, bit 8 "an LED was specified",
		 * bits 9.. the LED GPIO. */
		uint32_t gpio_mask = 0;
		if (request->wValue & 0x100u)
			gpio_mask = 1ul << ((request->wValue >> 9u) & 0x1fu);
		reset_usb_boot(gpio_mask, request->wValue & 0x7fu);
		/* does not return */
		return true;
	}

	case RESET_REQUEST_FLASH:
		/* ACK before rebooting: the delay exists so the status stage can
		 * actually leave the device. */
		tud_control_status(rhport, request);
		watchdog_reboot(0, 0, USB_RESET_TO_FLASH_DELAY_MS);
		return true;

	default:
		return false;
	}
}

static bool resetd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	return usb_reset_handle_request(rhport, stage, request);
}

static bool resetd_xfer_cb(uint8_t __unused rhport, uint8_t __unused ep_addr,
			   xfer_result_t __unused result, uint32_t __unused xferred_bytes)
{
	return true;
}

usbd_class_driver_t const usb_reset_driver =
{
		.init = resetd_init,
		.deinit = NULL,
		.reset = resetd_reset,
		.open = resetd_open,
		.control_xfer_cb = resetd_control_xfer_cb,
		.xfer_cb = resetd_xfer_cb,
		.sof = NULL,
#if CFG_TUSB_DEBUG >= 2
		.name = "RPI RESET"
#endif
};
