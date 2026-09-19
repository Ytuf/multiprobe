/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "probe.h"  // probe_deinit() for cooperative-exit HiZ on cable yank

#define WR_IDX(x) (x.wptr % DAP_PACKET_COUNT)
#define RD_IDX(x) (x.rptr % DAP_PACKET_COUNT)

#define WR_SLOT_PTR(x) &(x.data[WR_IDX(x)][0])
#define RD_SLOT_PTR(x) &(x.data[RD_IDX(x)][0])

/* Bounded wait for ring_mutex from the tud_task context (dap_edpt_xfer_cb,
 * dap_suspend_thread). Nothing running in tud_task may wait on this lock
 * indefinitely: with INCLUDE_vTaskSuspend == 1 an xSemaphoreTake(portMAX_DELAY)
 * can only return pdPASS or block forever (xTaskCheckForTimeOut returns pdFALSE
 * unconditionally for portMAX_DELAY), so one stranded holder kills tud_task, and
 * with it USB enumeration, until the next power cycle.
 * The lock is only ever held across a memcpy and one usbd_edpt_xfer(), and a
 * dap_thread that holds it inherits the priority of the waiting tud_task, so
 * 2 ms (40 ticks at configTICK_RATE_HZ 20000) is far longer than any legitimate
 * hold and far shorter than any host timeout. */
#define DAP_RING_LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(2)

DAPEndpointState ep_states[MAX_DAP_PROBES] = {};

bool buffer_full(buffer_t *buffer)
{
	return ((buffer->wptr + 1) % DAP_PACKET_COUNT == buffer->rptr);
}

bool buffer_empty(buffer_t *buffer)
{
	return (buffer->wptr == buffer->rptr);
}

void dap_edpt_init(void) {
	for(int i=0;i<MAX_DAP_PROBES;i++)
	{
		struct DAPEndpointState* state = &ep_states[i];
		state->itf_num = 0;
		state->probeId = i;
		if (state->ring_mutex == NULL)
			state->ring_mutex = xSemaphoreCreateMutex();
	}
}

bool dap_edpt_deinit(void)
{
	for(int i=0;i<MAX_DAP_PROBES;i++)
	{	
		struct DAPEndpointState* state = &ep_states[i];
		memset(state->DAPRequestBuffer, 0, sizeof(state->DAPRequestBuffer));
		memset(state->DAPResponseBuffer, 0, sizeof(state->DAPResponseBuffer));
		state->USBRequestBuffer.wptr = state->USBRequestBuffer.rptr = 0;
		state->USBResponseBuffer.wptr = state->USBResponseBuffer.rptr = 0;
		// Indices are gone, so a deferred update from the old session must not run.
		state->in_cb_deferred = state->out_cb_deferred = false;
	}

	return true;
}

void dap_edpt_reset(uint8_t __unused rhport)
{
	for(int i=0;i<MAX_DAP_PROBES;i++)
	{	
		struct DAPEndpointState* state = &ep_states[i];	
		state->itf_num = 0;
		state->probeId = i;
	}
}

char * dap_cmd_string[] = {
	[ID_DAP_Info               ] = "DAP_Info",
	[ID_DAP_HostStatus         ] = "DAP_HostStatus",
	[ID_DAP_Connect            ] = "DAP_Connect",
	[ID_DAP_Disconnect         ] = "DAP_Disconnect",
	[ID_DAP_TransferConfigure  ] = "DAP_TransferConfigure",
	[ID_DAP_Transfer           ] = "DAP_Transfer",
	[ID_DAP_TransferBlock      ] = "DAP_TransferBlock",
	[ID_DAP_TransferAbort      ] = "DAP_TransferAbort",
	[ID_DAP_WriteABORT         ] = "DAP_WriteABORT",
	[ID_DAP_Delay              ] = "DAP_Delay",
	[ID_DAP_ResetTarget        ] = "DAP_ResetTarget",
	[ID_DAP_SWJ_Pins           ] = "DAP_SWJ_Pins",
	[ID_DAP_SWJ_Clock          ] = "DAP_SWJ_Clock",
	[ID_DAP_SWJ_Sequence       ] = "DAP_SWJ_Sequence",
	[ID_DAP_SWD_Configure      ] = "DAP_SWD_Configure",
	[ID_DAP_SWD_Sequence       ] = "DAP_SWD_Sequence",
	[ID_DAP_JTAG_Sequence      ] = "DAP_JTAG_Sequence",
	[ID_DAP_JTAG_Configure     ] = "DAP_JTAG_Configure",
	[ID_DAP_JTAG_IDCODE        ] = "DAP_JTAG_IDCODE",
	[ID_DAP_SWO_Transport      ] = "DAP_SWO_Transport",
	[ID_DAP_SWO_Mode           ] = "DAP_SWO_Mode",
	[ID_DAP_SWO_Baudrate       ] = "DAP_SWO_Baudrate",
	[ID_DAP_SWO_Control        ] = "DAP_SWO_Control",
	[ID_DAP_SWO_Status         ] = "DAP_SWO_Status",
	[ID_DAP_SWO_ExtendedStatus ] = "DAP_SWO_ExtendedStatus",
	[ID_DAP_SWO_Data           ] = "DAP_SWO_Data",
	[ID_DAP_QueueCommands      ] = "DAP_QueueCommands",
	[ID_DAP_ExecuteCommands    ] = "DAP_ExecuteCommands",
};


uint16_t dap_edpt_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{

	TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass &&
			DAP_INTERFACE_SUBCLASS == itf_desc->bInterfaceSubClass &&
			DAP_INTERFACE_PROTOCOL == itf_desc->bInterfaceProtocol, 0);

	if(itf_desc->bInterfaceNumber >= MAX_DAP_PROBES )
		return 0;

	struct DAPEndpointState* state = &ep_states[itf_desc->bInterfaceNumber];

	state->_rhport = rhport;

	//  Initialise circular buffer indices
	state->USBResponseBuffer.wptr = 0;
	state->USBResponseBuffer.rptr = 0;
	state->USBRequestBuffer.wptr = 0;
	state->USBRequestBuffer.rptr = 0;

	// Initialse full/empty flags
	state->USBResponseBuffer.wasFull = false;
	state->USBResponseBuffer.wasEmpty = true;
	state->USBRequestBuffer.wasFull = false;
	state->USBRequestBuffer.wasEmpty = true;

	// A deferred update from the previous session would bump the indices we
	// just reset, so drop it with the session it belongs to.
	state->in_cb_deferred = false;
	state->out_cb_deferred = false;

	uint16_t const drv_len = sizeof(tusb_desc_interface_t) + (itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
	TU_VERIFY(max_len >= drv_len, 0);
	state->itf_num = itf_desc->bInterfaceNumber;

	// Initialising the OUT endpoint

	tusb_desc_endpoint_t *edpt_desc = (tusb_desc_endpoint_t *) (itf_desc + 1);
	uint8_t ep_addr = edpt_desc->bEndpointAddress;

	state->_out_ep_addr = ep_addr;

	// The OUT endpoint requires a call to usbd_edpt_xfer to initialise the endpoint, giving tinyUSB a buffer to consume when a transfer occurs at the endpoint
	usbd_edpt_open(rhport, edpt_desc);
	usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(state->USBRequestBuffer), DAP_PACKET_SIZE);

	// Initiliasing the IN endpoint

	edpt_desc++;
	ep_addr = edpt_desc->bEndpointAddress;

	state->_in_ep_addr = ep_addr;

	// The IN endpoint doesn't need a transfer to initialise it, as this will be done by the main loop of dap_thread
	usbd_edpt_open(rhport, edpt_desc);

	/* We are being configured, so the bus is not suspended. If the host reset us
	 * instead of resuming, tud_resume_cb never ran, and tud_mount_cb will not
	 * resume the thread either (was_configured survives a reset - _usbd_reset()
	 * does not call tud_umount_cb), so a thread suspended by tud_suspend_cb
	 * would stay suspended and this probe would stay dead until a power cycle.
	 * vTaskResume() on a task that is not suspended is a no-op. */
	if (state->dap_taskhandle != NULL)
		vTaskResume(state->dap_taskhandle);

	return drv_len;

}

bool dap_edpt_control_xfer_cb(uint8_t __unused rhport, uint8_t stage, tusb_control_request_t const *request)
{
	return false;
}

// Manage USBResponseBuffer (request) write and USBRequestBuffer (response) read indices
bool dap_edpt_xfer_cb(uint8_t __unused rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	const uint8_t ep_dir = tu_edpt_dir(ep_addr);
	DAPEndpointState* state = getEndpointStateByEndpointAddr(ep_addr);
	if(!state)
	{		
		printf("invalid ep_addr %x\n", ep_addr);
		return false;
	}	

	if(ep_dir == TUSB_DIR_IN)
	{
		if(xferred_bytes >= 0u && xferred_bytes <= DAP_PACKET_SIZE)
		{
			/* Bounded take (see DAP_RING_LOCK_TIMEOUT_TICKS); give only if it
			 * succeeded, as a give from a non-owner silently corrupts the mutex
			 * (configASSERT is compiled out in Release).
			 * If the lock is busy, hand the ring update to dap_thread rather
			 * than touch the ring unlocked: the IN transfer that just completed
			 * is the only one outstanding on this endpoint and no new one is
			 * queued until that update runs, so at most one can be pending. */
			bool locked = false;
			if (state->ring_mutex != NULL)
			{
				locked = (xSemaphoreTake(state->ring_mutex, DAP_RING_LOCK_TIMEOUT_TICKS) == pdTRUE);
				if (!locked)
				{
					state->in_cb_deferred = true;
					if (state->dap_taskhandle != NULL)
						xTaskNotifyGive(state->dap_taskhandle);
					return true;
				}
			}

			state->USBResponseBuffer.rptr++;

			// This checks that the buffer was not empty in DAP thread, which means the next buffer was not queued up for the in endpoint callback
			// So, queue up the buffer at the new read index, since we expect read to catch up to write at this point.
			// It is possible for the read index to be multiple spaces behind the write index (if the USB callbacks are lagging behind dap thread),
			// so we account for this by only setting wasEmpty to true if the next callback will empty the buffer
			if(!state->USBResponseBuffer.wasEmpty)
			{
				uint16_t slot_len = state->resp_lens[RD_IDX(state->USBResponseBuffer)];
				usbd_edpt_xfer(rhport, ep_addr, RD_SLOT_PTR(state->USBResponseBuffer), slot_len);
				state->USBResponseBuffer.wasEmpty = (state->USBResponseBuffer.rptr + 1) == state->USBResponseBuffer.wptr;
			}

			if (locked)
				xSemaphoreGive(state->ring_mutex);

			// Handle can be NULL between unmount and next mount; xTaskNotifyGive(NULL) asserts.
			if (state->dap_taskhandle != NULL)
				xTaskNotifyGive(state->dap_taskhandle);
			return true;
		}

		return false;

	} else if(ep_dir == TUSB_DIR_OUT)    {

		if(xferred_bytes >= 0u && xferred_bytes <= DAP_PACKET_SIZE)
		{
			/* Same bounded take as the IN case. The received packet is already
			 * in the write slot; only the wptr bump that publishes it and the
			 * re-arming transfer are deferred, so nothing is lost or reordered
			 * and no further OUT completion can arrive until dap_thread has run
			 * the deferred update. */
			bool locked = false;
			if (state->ring_mutex != NULL)
			{
				locked = (xSemaphoreTake(state->ring_mutex, DAP_RING_LOCK_TIMEOUT_TICKS) == pdTRUE);
				if (!locked)
				{
					state->out_cb_deferred = true;
					if (state->dap_taskhandle != NULL)
						xTaskNotifyGive(state->dap_taskhandle);
					return true;
				}
			}

			// Only queue the next buffer in the out callback if the buffer is not full
			// If full, we set the wasFull flag, which will be checked by dap thread
			if(!buffer_full(&state->USBRequestBuffer))
			{
				state->USBRequestBuffer.wptr++;
				usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(state->USBRequestBuffer), DAP_PACKET_SIZE);
				state->USBRequestBuffer.wasFull = false;
			}
			else {
				state->USBRequestBuffer.wasFull = true;
			}

			if (locked)
				xSemaphoreGive(state->ring_mutex);

			if (state->dap_taskhandle != NULL)
				xTaskNotifyGive(state->dap_taskhandle);
			return true;
		}

		return false;
	}
	else return false;
}

/* Ring updates that dap_edpt_xfer_cb could not do itself because the lock was
 * busy. Runs the exact same body, in dap_thread, under the same lock - only
 * later - so the ring stays consistent and no packet is dropped. */
static void dap_service_deferred_cb(DAPEndpointState* state)
{
	if (!state->in_cb_deferred && !state->out_cb_deferred)
		return;

	bool locked = false;
	if (state->ring_mutex != NULL)
	{
		/* Bounded here too: if it fails the flags stay set and the notify wait
		 * below stays short, so this is retried instead of being lost. */
		locked = (xSemaphoreTake(state->ring_mutex, DAP_RING_LOCK_TIMEOUT_TICKS) == pdTRUE);
		if (!locked)
			return;
	}

	if (state->out_cb_deferred)
	{
		state->out_cb_deferred = false;

		if(!buffer_full(&state->USBRequestBuffer))
		{
			state->USBRequestBuffer.wptr++;
			usbd_edpt_xfer(state->_rhport, state->_out_ep_addr, WR_SLOT_PTR(state->USBRequestBuffer), DAP_PACKET_SIZE);
			state->USBRequestBuffer.wasFull = false;
		}
		else {
			state->USBRequestBuffer.wasFull = true;
		}
	}

	if (state->in_cb_deferred)
	{
		state->in_cb_deferred = false;

		state->USBResponseBuffer.rptr++;

		if(!state->USBResponseBuffer.wasEmpty)
		{
			uint16_t slot_len = state->resp_lens[RD_IDX(state->USBResponseBuffer)];
			usbd_edpt_xfer(state->_rhport, state->_in_ep_addr, RD_SLOT_PTR(state->USBResponseBuffer), slot_len);
			state->USBResponseBuffer.wasEmpty = (state->USBResponseBuffer.rptr + 1) == state->USBResponseBuffer.wptr;
		}
	}

	if (locked)
		xSemaphoreGive(state->ring_mutex);
}


/* Wait for a callback to wake us - but never indefinitely while a callback is
 * still waiting on us to finish its ring update, as nothing else would wake us:
 * a deferred OUT update owns the wptr bump the consumer loop waits for. */
static void dap_wait_for_cb(DAPEndpointState* state)
{
	dap_service_deferred_cb(state);

	ulTaskNotifyTake(pdTRUE, (state->in_cb_deferred || state->out_cb_deferred)
	                         ? DAP_RING_LOCK_TIMEOUT_TICKS : portMAX_DELAY);
}


/* tud_suspend_cb used to call vTaskSuspend() on dap_thread unguarded. Suspending
 * it inside a locked region strands ring_mutex, and if the host resets instead
 * of resuming (tud_resume_cb never runs; _usbd_reset() does not call
 * tud_umount_cb, so was_configured stays 1 and tud_mount_cb will not resume it
 * either) the lock is never released - which used to wedge tud_task in
 * dap_edpt_xfer_cb for the rest of the power cycle. Take the lock first so the
 * target cannot be inside a locked region.
 * Unlike cdc_uart_suspend_thread, a failed take does NOT suspend anyway: this
 * suspend is only a power saving, and dap_thread is parked on a notification for
 * the whole bus suspend regardless (no callbacks arrive), so skipping it costs
 * nothing, while suspending a possible lock holder costs exactly the failure
 * above. */
void dap_suspend_thread(DAPEndpointState* state)
{
	if (state->dap_taskhandle == NULL)
		return;

	if (state->ring_mutex == NULL)  // no lock exists, so none can be stranded
	{
		vTaskSuspend(state->dap_taskhandle);
		return;
	}

	if (xSemaphoreTake(state->ring_mutex, DAP_RING_LOCK_TIMEOUT_TICKS) != pdTRUE)
		return;

	vTaskSuspend(state->dap_taskhandle);
	xSemaphoreGive(state->ring_mutex);
}


void dap_thread(void *ptr)
{
	struct DAPEndpointState* state = (struct DAPEndpointState*)ptr;
	uint32_t n;
	do
	{
		if (state->should_exit)  // cooperative exit; top of loop owns no mutexes
		{
			probe_deinit(state->probeId);  // HiZ pads; cable yank skips DAP_Disconnect
			state->dap_taskhandle = NULL;
			vTaskDelete(NULL);
			/* unreachable */
		}

		// Publish anything a callback handed over before consuming the ring.
		dap_service_deferred_cb(state);
		while(state->USBRequestBuffer.rptr != state->USBRequestBuffer.wptr)
		{
			/*
			 * Atomic command support - buffer QueueCommands, but don't process them
			 * until a non-QueueCommands packet is seen.
			 */
			n = state->USBRequestBuffer.rptr;
			while (state->USBRequestBuffer.data[n % DAP_PACKET_COUNT][0] == ID_DAP_QueueCommands) {
				probe_info("%lu %lu DAP queued cmd %s len %02x\n",
					       state->USBRequestBuffer.wptr, state->USBRequestBuffer.rptr,
					       dap_cmd_string[state->USBRequestBuffer.data[n % DAP_PACKET_COUNT][0]], state->USBRequestBuffer.data[n % DAP_PACKET_COUNT][1]);
				state->USBRequestBuffer.data[n % DAP_PACKET_COUNT][0] = ID_DAP_ExecuteCommands;
				n++;
				while (n == state->USBRequestBuffer.wptr) {
					/* Need yield in a loop here, as IN callbacks will also wake the thread */
					probe_info("DAP wait\n");
					dap_wait_for_cb(state);
				}
			}
			// Read a single packet from the USB buffer into the DAP Request buffer
			memcpy(state->DAPRequestBuffer, RD_SLOT_PTR(state->USBRequestBuffer), DAP_PACKET_SIZE);
			/*probe_info("%lu %lu DAP cmd %s len %02x\n",
				       state->USBRequestBuffer.wptr, state->USBRequestBuffer.rptr,
				       dap_cmd_string[DAPRequestBuffer[0]], DAPRequestBuffer[1]);*/
			state->USBRequestBuffer.rptr++;

			// If the buffer was full in the out callback, we need to queue up another buffer for the endpoint to consume, now that we know there is space in the buffer.
			if(state->USBRequestBuffer.wasFull)
			{
				/* dap_thread is not tud_task, so portMAX_DELAY is acceptable
				 * here: the only other taker is dap_edpt_xfer_cb, whose hold is
				 * a memcpy plus one usbd_edpt_xfer(). With INCLUDE_vTaskSuspend
				 * == 1 such a take cannot fail, so the flag only covers a NULL
				 * mutex (creation failed at boot under heap_1). */
				bool locked = (state->ring_mutex != NULL) &&
				              (xSemaphoreTake(state->ring_mutex, portMAX_DELAY) == pdTRUE);
				state->USBRequestBuffer.wptr++;
				usbd_edpt_xfer(state->_rhport, state->_out_ep_addr, WR_SLOT_PTR(state->USBRequestBuffer), DAP_PACKET_SIZE);
				state->USBRequestBuffer.wasFull = false;
				if (locked)
					xSemaphoreGive(state->ring_mutex);
			}

			uint32_t resp_len = DAP_ExecuteCommand(state->probeId, state->DAPRequestBuffer, state->DAPResponseBuffer);
			/*probe_info("%lu %lu DAP resp %s\n",
					USBResponseBuffer.wptr, USBResponseBuffer.rptr,
					dap_cmd_string[DAPResponseBuffer[0]]);*/


			/* portMAX_DELAY for the same reason as above; the flag only covers a
			 * NULL mutex. */
			bool respLocked = (state->ring_mutex != NULL) &&
			                  (xSemaphoreTake(state->ring_mutex, portMAX_DELAY) == pdTRUE);

			// Capture slot index before bumping wptr so IN cb sees matching resp_lens[].
			uint32_t wr_slot = WR_IDX(state->USBResponseBuffer);
			memcpy(&state->USBResponseBuffer.data[wr_slot][0], state->DAPResponseBuffer, (uint16_t) resp_len);
			state->resp_lens[wr_slot] = (uint16_t) resp_len;

			if(buffer_empty(&state->USBResponseBuffer))
			{
				state->USBResponseBuffer.wptr++;

				usbd_edpt_xfer(state->_rhport, state->_in_ep_addr, &state->USBResponseBuffer.data[wr_slot][0], (uint16_t) resp_len);
			} else {

				state->USBResponseBuffer.wptr++;

				// The In callback needs to check this flag to know when to queue up the next buffer.
				state->USBResponseBuffer.wasEmpty = false;
			}
			if (respLocked)
				xSemaphoreGive(state->ring_mutex);
		}

		// Suspend DAP thread until it is awoken by a USB thread callback
		dap_wait_for_cb(state);

	} while (1);

}

usbd_class_driver_t const _dap_edpt_driver =
{
		.init = dap_edpt_init,
		.deinit = dap_edpt_deinit,
		.reset = dap_edpt_reset,
		.open = dap_edpt_open,
		.control_xfer_cb = dap_edpt_control_xfer_cb,
		.xfer_cb = dap_edpt_xfer_cb,
		.sof = NULL,
#if CFG_TUSB_DEBUG >= 2
		.name = "DAP ENDPOINT"
#endif
};

// Add the custom driver to the tinyUSB stack
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = 1;
	return &_dap_edpt_driver;
}

/*void dap_close_all(void) {
	for(int i=0;i<MAX_DAP_PROBES;i++)
	{
		struct DAPEndpointState* state = &ep_states[i];
		
		usbd_edpt_close(state->_rhport, state->_in_ep_addr);
		usbd_edpt_close(state->_rhport, state->_out_ep_addr);
	}
}
*/


DAPEndpointState* getEndpointStateByEndpointAddr(uint8_t ep_addr)
{
	for(int i=0;i<MAX_DAP_PROBES;i++)
	{
		struct DAPEndpointState* state = &ep_states[i];
		// Both ep addrs zero == torn-down state; skip so stale cb can't match.
		if(state->_out_ep_addr == 0 && state->_in_ep_addr == 0)
			continue;
		if(state->_out_ep_addr == ep_addr)
			return state;
		if(state->_in_ep_addr == ep_addr)
			return state;
	}
	return 0;
}


DAPEndpointState* getEndpointStateByIndex(uint8_t index)
{
	return &ep_states[index];
}


DAPEndpointState* getEndpointStateByITF(uint8_t itf)
{
	for(int i=0;i<MAX_DAP_PROBES;i++)
	{
		struct DAPEndpointState* state = &ep_states[i];
		if(state->itf_num == itf)
			return state;
	}
	return 0;
}