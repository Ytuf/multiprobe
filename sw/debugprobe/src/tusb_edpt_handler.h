/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TUSB_EDPT_HANDLER_H
#define TUSB_EDPT_HANDLER_H

#include "tusb.h"

#include "device/usbd_pvt.h"
#include "DAP_config.h"

#include "FreeRTOS.h"
#include "semphr.h"

#define DAP_INTERFACE_SUBCLASS 0x00
#define DAP_INTERFACE_PROTOCOL 0x00

typedef struct {
	uint8_t data[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
	volatile uint32_t wptr;
	volatile uint32_t rptr;
	volatile bool wasEmpty;
	volatile bool wasFull;
} buffer_t;

extern TaskHandle_t tud_taskhandle;

typedef struct DAPEndpointState
{
	uint32_t probeId;

	uint8_t itf_num;
	uint8_t _rhport;

	volatile uint16_t resp_lens[DAP_PACKET_COUNT];

	volatile uint8_t _out_ep_addr;
	volatile uint8_t _in_ep_addr;

	buffer_t USBRequestBuffer;
	buffer_t USBResponseBuffer;

	uint8_t DAPRequestBuffer[DAP_PACKET_SIZE];
	uint8_t DAPResponseBuffer[DAP_PACKET_SIZE];

	TaskHandle_t dap_taskhandle;

	/* Cooperative exit: set by tud_unmount_cb, observed at top of dap_thread loop. */
	volatile bool should_exit;

	/* Serializes ring updates between dap_thread and xfer_cb; usbd_edpt_xfer
	 * cannot be called with the scheduler suspended (it takes osal mutexes). */
	SemaphoreHandle_t ring_mutex;

	/* Ring update handed to dap_thread by dap_edpt_xfer_cb when it could not
	 * take ring_mutex in time (it runs in tud_task and must not wait long).
	 * At most one can be outstanding per direction: no new transfer is queued on
	 * that endpoint until the deferred update has run. */
	volatile bool in_cb_deferred;
	volatile bool out_cb_deferred;
} DAPEndpointState;

extern DAPEndpointState ep_states[MAX_DAP_PROBES];

/* Main DAP loop */
void dap_thread(void *ptr);

/* Suspends state->dap_taskhandle without stranding ring_mutex; does nothing if
 * the lock cannot be taken in time (see the comment on the definition). */
void dap_suspend_thread(DAPEndpointState* state);

/* Endpoint Handling */
void dap_edpt_init(void);
uint16_t dap_edpt_open(uint8_t __unused rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len);
bool dap_edpt_control_xfer_cb(uint8_t __unused rhport, uint8_t stage,  tusb_control_request_t const *request);
bool dap_edpt_xfer_cb(uint8_t __unused rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);

/* Helper Functions */
bool buffer_full(buffer_t *buffer);
bool buffer_empty(buffer_t *buffer);

/** Endpoint api  */
DAPEndpointState* getEndpointStateByITF(uint8_t itf);
DAPEndpointState* getEndpointStateByEndpointAddr(uint8_t ep_addr);
DAPEndpointState* getEndpointStateByIndex(uint8_t index);

#endif
