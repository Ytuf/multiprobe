/*
 * Raspberry Pi USB reset interface (picotool "-f" support).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef USB_RESET_H
#define USB_RESET_H

#include "tusb.h"
#include "device/usbd_pvt.h"

/* RESET_INTERFACE_SUBCLASS / RESET_INTERFACE_PROTOCOL /
 * RESET_REQUEST_BOOTSEL / RESET_REQUEST_FLASH */
#include "pico/usb_reset_interface.h"

/* Sentinel for "interface not opened yet". 0 is a real interface number here
 * (ITF_NUM_PROBE), so the SDK's choice of 0 would let a request aimed at the
 * first CMSIS-DAP interface be mistaken for a reset request before the reset
 * interface has ever been configured. */
#define USB_RESET_ITF_INVALID 0xffu

/* Matches PICO_STDIO_USB_RESET_RESET_TO_FLASH_DELAY_MS: long enough for the
 * control status stage to reach the host before the chip resets. */
#ifndef USB_RESET_TO_FLASH_DELAY_MS
#define USB_RESET_TO_FLASH_DELAY_MS 100
#endif

/* Registered with TinyUSB alongside the DAP driver (usbd_app_driver_get_cb). */
extern usbd_class_driver_t const usb_reset_driver;

/* Interface number the host must address, or USB_RESET_ITF_INVALID. */
uint8_t usb_reset_itf_num(void);

/* Handles RESET_REQUEST_BOOTSEL / RESET_REQUEST_FLASH aimed at the reset
 * interface. Returns false (stall) for anything else, so it is safe to call
 * as the first link of a control-transfer chain. */
bool usb_reset_handle_request(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request);

#endif /* USB_RESET_H */
