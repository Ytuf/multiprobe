/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_edpt_handler.h"
#include "DAP.h"

#define WR_IDX(x) (x.wptr % DAP_PACKET_COUNT)
#define RD_IDX(x) (x.rptr % DAP_PACKET_COUNT)

#define WR_SLOT_PTR(x) &(x.data[WR_IDX(x)][0])
#define RD_SLOT_PTR(x) &(x.data[RD_IDX(x)][0])

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
		/* Fix #15: create the ring mutex exactly once. dap_edpt_init is the
		 * tinyUSB class-driver init hook, invoked from tusb_init() in main()
		 * BEFORE vTaskStartScheduler(). xSemaphoreCreateMutex is safe to call
		 * pre-scheduler (it only allocates from the heap and initialises the
		 * queue struct). Guard against a second init pass (re-enumeration)
		 * leaking a mutex by only creating when NULL. */
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

	// Record the root hub port so later usbd_edpt_xfer calls target the right controller
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
			/* Fix #15: serialise ring-index updates and the usbd_edpt_xfer
			 * re-arm against the producer in dap_thread. Skip the lock if
			 * ring_mutex hasn't been created yet (shouldn't happen --
			 * dap_edpt_init runs before the scheduler) to stay NULL-safe. */
			if (state->ring_mutex != NULL)
				xSemaphoreTake(state->ring_mutex, portMAX_DELAY);

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

			if (state->ring_mutex != NULL)
				xSemaphoreGive(state->ring_mutex);

			/* Fix #11c: dap_taskhandle can be NULL between tud_unmount_cb
			 * (sets should_exit) and the next tud_mount_cb. xTaskNotifyGive(NULL)
			 * trips configASSERT, so just drop the notify if the task is gone.
			 * Fix #15: notify outside the mutex -- task notifications don't
			 * need ring_mutex and keeping them outside shortens the critical
			 * section. */
			if (state->dap_taskhandle != NULL)
				xTaskNotifyGive(state->dap_taskhandle);
			return true;
		}

		return false;

	} else if(ep_dir == TUSB_DIR_OUT)    {

		if(xferred_bytes >= 0u && xferred_bytes <= DAP_PACKET_SIZE)
		{
			/* Fix #15: serialise against dap_thread, which also reads
			 * wasFull and bumps wptr / re-arms the OUT xfer when it drains
			 * a full buffer. */
			if (state->ring_mutex != NULL)
				xSemaphoreTake(state->ring_mutex, portMAX_DELAY);

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

			if (state->ring_mutex != NULL)
				xSemaphoreGive(state->ring_mutex);

			/* Fix #11c: see note above; guard against NULL handle.
			 * Fix #15: notify outside the mutex. */
			if (state->dap_taskhandle != NULL)
				xTaskNotifyGive(state->dap_taskhandle);
			return true;
		}

		return false;
	}
	else return false;
}

void dap_thread(void *ptr)
{
	struct DAPEndpointState* state = (struct DAPEndpointState*)ptr;
	uint32_t n;
	do
	{
		/* Fix #11a: cooperative exit. tud_unmount_cb sets should_exit and
		 * notifies us; at the top of the outer loop we own no mutexes and
		 * the scheduler is not suspended, so it is safe to self-delete.
		 * Self-delete (vs vTaskDelete from another task) ensures any
		 * mutexes/scheduler-suspends taken inside the inner block have
		 * already been released by us. */
		if (state->should_exit)
		{
			state->dap_taskhandle = NULL;
			vTaskDelete(NULL);
			/* unreachable */
		}
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
					ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
				/* Fix #15: use the per-state ring_mutex instead of
				 * vTaskSuspendAll/xTaskResumeAll. usbd_edpt_xfer internally
				 * takes osal mutexes, and FreeRTOS forbids mutex acquisition
				 * while the scheduler is suspended. The mutex provides the
				 * same mutual exclusion against the OUT xfer_cb without that
				 * illegality. */
				if (state->ring_mutex != NULL)
					xSemaphoreTake(state->ring_mutex, portMAX_DELAY);
				state->USBRequestBuffer.wptr++;
				usbd_edpt_xfer(state->_rhport, state->_out_ep_addr, WR_SLOT_PTR(state->USBRequestBuffer), DAP_PACKET_SIZE);
				state->USBRequestBuffer.wasFull = false;
				if (state->ring_mutex != NULL)
					xSemaphoreGive(state->ring_mutex);
			}

			uint32_t resp_len = DAP_ExecuteCommand(state->probeId, state->DAPRequestBuffer, state->DAPResponseBuffer);
			/*probe_info("%lu %lu DAP resp %s\n",
					USBResponseBuffer.wptr, USBResponseBuffer.rptr,
					dap_cmd_string[DAPResponseBuffer[0]]);*/


			/* Fix #15: per-state ring_mutex replaces vTaskSuspendAll. See
			 * the matching comment on the wasFull branch above and the
			 * mutex creation in dap_edpt_init for the full rationale --
			 * usbd_edpt_xfer takes osal mutexes internally and cannot be
			 * called with the scheduler suspended. The mutex serialises
			 * this producer block against the IN xfer_cb consumer block. */
			if (state->ring_mutex != NULL)
				xSemaphoreTake(state->ring_mutex, portMAX_DELAY);

			// Compute the slot index BEFORE bumping wptr so the IN-callback consumer
			// can read the matching length from resp_lens[] for the slot it is sending.
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
			if (state->ring_mutex != NULL)
				xSemaphoreGive(state->ring_mutex);
		}

		// Suspend DAP thread until it is awoken by a USB thread callback
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

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
		/* Fix #11c: tud_unmount_cb zeros _out_ep_addr/_in_ep_addr so a
		 * stale xfer_cb after teardown cannot accidentally match against
		 * a torn-down state. Both being zero means "not open". */
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