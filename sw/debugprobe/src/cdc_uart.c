/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2025 Picolemon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "tusb.h"
#include <stdlib.h>

#include "cdc_uart.h"
#include "probe_config.h"

SemaphoreHandle_t globalMutex;
extern TaskHandle_t uart_taskHandle;

/* Upper bound on how long any caller waits for the UART hardware lock.
 * configTICK_RATE_HZ is 20000, so portTICK_PERIOD_MS is (1000 / 20000) == 0 by
 * integer division: the old "portTICK_PERIOD_MS * 1000" timeout was a zero tick
 * (non-blocking) try that never waited. pdMS_TO_TICKS is exact at this tick rate
 * (10 * 20000 / 1000 == 200 ticks) and TickType_t is 32 bit here
 * (configUSE_16_BIT_TICKS 0), so there is no overflow.
 * Nothing waits forever: a stranded holder must never be able to block tud_task
 * and with it USB enumeration. */
#define HW_UART_LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(10)


/* Every UART register access in this file runs under this lock.
 * uart_deinit() is uart_reset() in the SDK: it asserts the peripheral reset and
 * leaves the block in reset until the matching uart_init(). A register access
 * from any other task inside that window faults the bus -> HardFault -> the
 * default handler spins, while the USB pull-up stays asserted in hardware, so
 * the host sees an attached device that answers nothing.
 * Returns true only if the lock was actually taken; only a successful taker may
 * call unlock_hw_uart(). A give from a non-owner clears xMutexHolder and
 * decrements the real owner's uxMutexesHeld, and configASSERT is compiled out in
 * Release (-DNDEBUG), so that corruption would be silent. */
static bool lock_hw_uart(TickType_t timeoutTicks)
{
  if( globalMutex == NULL )
    return false;

  return xSemaphoreTake( globalMutex, timeoutTicks ) == pdTRUE;
}


static void unlock_hw_uart(void)
{
  xSemaphoreGive( globalMutex );
}


static bool cdc_uart_hw_live(const struct CDCUartInstance_t* uartInst)
{
  return uartInst->isUartInitialised && uartInst->uart != NULL;
}


/* Caller must hold the hw lock and must have checked cdc_uart_hw_live(), i.e.
 * that the block is bound and out of reset. */
static void cdc_uart_drain_rx(struct CDCUartInstance_t* uartInst)
{
  uint32_t readCnt = 0;

  while(uart_is_readable(uartInst->uart) && readCnt < 32 )
  {
    uint8_t c = (uint8_t)uart_get_hw(uartInst->uart)->dr;
    uartInst->buffer[ uartInst->writingBuffer ] = c;
    uartInst->writingBuffer = (uartInst->writingBuffer + 1) % CDC_BUFFER_SZ;

    readCnt++;
  }
}


void cdc_uart_core2( void* ptr)
{
    struct CDCUartInstance_t* uartInst0 = &uartDevices[0];
    struct CDCUartInstance_t* uartInst1 = &uartDevices[1];

    while (true)
    {
      tight_loop_contents();

      /* This is the highest priority task in the system and it polls every tick,
       * so it is the task that preempts into a uart_deinit()/uart_init() window.
       * It must never touch a UART without holding the lock, and never one that
       * is not initialised: an unbound instance can still hold a non-NULL uart
       * pointer (a profile with rxPin/txPin == -1 sets uart->uart and then
       * returns before uart_init()). */
      if( lock_hw_uart( HW_UART_LOCK_TIMEOUT_TICKS ) )
      {
        if( cdc_uart_hw_live(uartInst0) )
          cdc_uart_drain_rx(uartInst0);

        /* Same hw uart bound twice: already drained above. */
        if( cdc_uart_hw_live(uartInst1) && uartInst1->uart != uartInst0->uart )
          cdc_uart_drain_rx(uartInst1);

        unlock_hw_uart();
      }

      TickType_t wake;
      wake = xTaskGetTickCount();
      xTaskDelayUntil(&wake, 1);
    }
}


/* Returns false only when the hw lock could not be taken, i.e. "try again
 * later"; the caller must keep the pending profile so the port is not stranded.
 * A profile with no pins is not an error: there is simply nothing to bind. */
bool cdc_uart_init(struct CDCUartInstance_t* uart, const struct UartProfile_t* profile)
{
  if(!profile)
    return true;

  if(!lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS))
    return false;

  if(uart->isUartInitialised)
  {
    unlock_hw_uart();
    return true;
  }

  switch (profile->hwUartId)
  {   
  case 0:
    uart->uart = uart0;
    break;
  case 1:
    uart->uart = uart1;    
    break;
  }

  uart->currentProfile = profile;
  uart->nextProfile = 0;
  uart->baud = profile->baud;  
  uart->rxPin = profile->rxPin;
  uart->txPin = profile->txPin;

  memset(uart->matchKeyStates, 0, sizeof(uart->matchKeyStates));
  if(uart->rxPin == -1 || uart->txPin == -1)
  {
    unlock_hw_uart();
    return true;
  }
  gpio_set_function(uart->rxPin, GPIO_FUNC_UART);
  gpio_set_function(uart->txPin, GPIO_FUNC_UART);
  gpio_set_pulls(uart->rxPin, 1, 0);
  uart_init(uart->uart, uart->baud);

#ifdef PROBE_UART_HWFC
    // HWFC implies that hardware flow control is implemented and the
    // UART operates in "full-duplex" mode (See USB CDC PSTN120 6.3.12).
    // Default to pulling in the active direction, so an unconnected CTS
    // behaves the same as if CTS were not enabled. 
    gpio_set_pulls(uart->ctsPin, 0, 1);
    gpio_set_function(uart->rtsPin, GPIO_FUNC_UART);
    gpio_set_function(uart->ctsPin, GPIO_FUNC_UART);
    uart_set_hw_flow(PROBE_UART_INTERFACE, true, true);
#else
#ifdef PROBE_UART_RTS
    gpio_init(uart->rtsPin);
    gpio_set_dir(uart->rtsPin, GPIO_OUT);
    gpio_put(uart->rtsPin, 1);
#endif
#endif

#ifdef PROBE_UART_DTR
    gpio_init(PROBE_UART_DTR);
    gpio_set_dir(PROBE_UART_DTR, GPIO_OUT);
    gpio_put(PROBE_UART_DTR, 1);
#endif

  uart->isUartInitialised = true;

  unlock_hw_uart();
  return true;
}


/* Returns false only when the hw lock could not be taken (see cdc_uart_init). */
bool cdc_uart_deinit(struct CDCUartInstance_t* uart)
{
  if(!lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS))
    return false;

  if(!uart->isUartInitialised)
  {
    unlock_hw_uart();
    return true;
  }

  /* Publish "no hardware" before the block goes into reset. uart_deinit() leaves
   * the UART in reset, so anything that still saw a live instance here would
   * fault the bus on its next register access. Holders of the lock are already
   * excluded; this also protects the unlocked isUartInitialised checks
   * (tud_cdc_line_coding_cb / tud_cdc_line_state_cb / cdc_led_update). */
  uart_inst_t* hwUart = uart->uart;
  uart->uart = 0;
  uart->isUartInitialised = false;

  if(hwUart)
    uart_deinit(hwUart);

  if(uart->rxPin != -1)
    gpio_set_function(uart->rxPin, GPIO_FUNC_NULL);
  if(uart->txPin != -1)
    gpio_set_function(uart->txPin, GPIO_FUNC_NULL);

  uart->currentProfile = 0;
  uart->baud = -1;
  uart->rxPin = -1;
  uart->txPin = -1;

  unlock_hw_uart();
  return true;
}


// Custom vendor trigger
void cdc_activate_profile( int uartId, int profileId )
{
  if(uartId >= MAX_UART_DEVICES || profileId >= MAX_UART_PROFILES)
    return;

  struct CDCUartInstance_t* uartInst = &uartDevices[uartId];
  uartInst->nextProfile = &uartProfiles[profileId];
}


void cdc_handle_profile_switch(struct CDCUartInstance_t* uart)
{
  if(uart->nextProfile)
  {
    /* Snapshot: cdc_activate_profile() can retarget nextProfile from another
     * context while the switch is in progress. */
    const struct UartProfile_t* nextProfile = uart->nextProfile;

    // Release matching hw
    for(int i=0;i<MAX_UART_DEVICES;i++)
    {
      // Other device
      if(&uartDevices[i] != uart)
      {
        // Has profile
        if(uartDevices[i].currentProfile)
        {
          // Same hw
          if(uartDevices[i].currentProfile->hwUartId == nextProfile->hwUartId)
          {
            // Release it
            /* Keep nextProfile pending and retry on the next pass if the hw lock
             * is busy - dropping the request would strand the port. */
            if(!cdc_uart_deinit(&uartDevices[i]))
              return;
          }
        }
      }
    }



    // de-init current
    if(!cdc_uart_deinit(uart))
      return;

    // bind uart to profile
    if(!cdc_uart_init(uart, nextProfile))
      return;
  }

  uart->nextProfile = 0;
}


bool cdc_uart_task(struct CDCUartInstance_t* uart)
{
    // Profile switch
    cdc_handle_profile_switch(uart);

    // Ensure uart
    if(!uart->isUartInitialised)
      return false;

    bool keep_alive = false;

    if (tud_cdc_n_connected(uart->itf)) 
    {
        uart->was_connected = 1;
        int written = 0;
        
        if(uart->readingBuffer != uart->writingBuffer)
        {
          written = tud_cdc_n_write_available(uart->itf);

          while(written > 0 && uart->readingBuffer != uart->writingBuffer)
          {
            uart->lastActivityTimeRx = time_us_32();

            uint8_t nextC = uart->buffer[uart->readingBuffer];
            uart->readingBuffer = (uart->readingBuffer + 1) % CDC_BUFFER_SZ;

            tud_cdc_n_write(uart->itf, &nextC, 1);
          }

          tud_cdc_n_write_flush(uart->itf);          
        }

      /* Reading from a firehose and writing to a FIFO. */
      size_t watermark = MIN(tud_cdc_n_available(uart->itf), sizeof(uart->tx_buf));
      if (watermark > 0) {
        size_t tx_len;

        /* Batch up to half a FIFO of data - don't clog up on RX */
        watermark = MIN(watermark, 16);
        tx_len = tud_cdc_n_read(uart->itf, uart->tx_buf, watermark);

        /* The block can be in reset (uart_deinit()/uart_init() in
         * cdc_uart_line_coding_cb) while this task runs, so take the lock and
         * re-check under it. */
        if (tx_len && lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS)) {
          if (cdc_uart_hw_live(uart))
            uart_write_blocking(uart->uart, uart->tx_buf, tx_len);
          unlock_hw_uart();
        }

        uart->lastActivityTimeTx = time_us_32();
      }

      /* Pending break handling */
      if (uart->timed_break)
      {
        if (((int)uart->break_expiry - (int)xTaskGetTickCount()) < 0) {
          if (lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS)) {
            if (cdc_uart_hw_live(uart))
              uart_set_break(uart->uart, false);
            unlock_hw_uart();
            uart->timed_break = false;
          }
          else
          {
            /* Could not clear the break yet - stay hot and retry. */
            keep_alive = true;
          }
        }
        else
        {
          keep_alive = true;
        }
      }
    }
    else if (uart->was_connected)
    {
      tud_cdc_n_write_clear(uart->itf);

      if (lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS)) {
        if (cdc_uart_hw_live(uart))
          uart_set_break(uart->uart, false);
        unlock_hw_uart();
      }

      uart->timed_break = false;
      uart->was_connected = 0;
      uart->cdc_tx_oe = 0;
    }

    return keep_alive;
}


void cdc_uart_thread(void *ptr)
{
  struct CDCUartInstance_t* uart = (CDCUartInstance_t*)ptr;
  if( !uart->isConfigured )
    return;  
  BaseType_t delayed;
  uart->last_wake = xTaskGetTickCount();
  bool keep_alive;
  /* Threaded with a polling interval that scales according to linerate */
  while (1) {
    keep_alive = cdc_uart_task(uart);
    if (!keep_alive) {
      delayed = xTaskDelayUntil(&uart->last_wake, uart->interval);
        if (delayed == pdFALSE)
          uart->last_wake = xTaskGetTickCount();
    }
  }
}


void cdc_uart_line_coding_cb(struct CDCUartInstance_t* uart, uint8_t itf, cdc_line_coding_t const* line_coding)
{  
  uart_parity_t parity;
  uint data_bits, stop_bits;
  /* Set the tick thread interval to the amount of time it takes to
   * fill up half a FIFO. Millis is too coarse for integer divide.
   */
  uint32_t micros = (1000 * 1000 * 16 * 10) / MAX(line_coding->bit_rate, 1);
  uart->interval = MAX(1, micros / ((1000 * 1000) / configTICK_RATE_HZ));
  probe_info("New baud rate %ld micros %ld interval %lu\n",
                  line_coding->bit_rate, micros, uart->interval);

  tud_cdc_n_write_clear(uart->itf);
  tud_cdc_n_read_flush(uart->itf);

  switch (line_coding->parity) {
  case CDC_LINE_CODING_PARITY_ODD:
    parity = UART_PARITY_ODD;
    break;
  case CDC_LINE_CODING_PARITY_EVEN:
    parity = UART_PARITY_EVEN;
    break;
  default:
    probe_info("invalid parity setting %u\n", line_coding->parity);
    /* fallthrough */
  case CDC_LINE_CODING_PARITY_NONE:
    parity = UART_PARITY_NONE;
    break;
  }

  switch (line_coding->data_bits) {
  case 5:
  case 6:
  case 7:
  case 8:
    data_bits = line_coding->data_bits;
    break;
  default:
    probe_info("invalid data bits setting: %u\n", line_coding->data_bits);
    data_bits = 8;
    break;
  }

  /* The PL011 only supports 1 or 2 stop bits. 1.5 stop bits is translated to 2,
   * which is safer than the alternative. */
  switch (line_coding->stop_bits) {
  case CDC_LINE_CONDING_STOP_BITS_1_5:
  case CDC_LINE_CONDING_STOP_BITS_2:
    stop_bits = 2;
  break;
  default:
    probe_info("invalid stop bits setting: %u\n", line_coding->stop_bits);
    /* fallthrough */
  case CDC_LINE_CONDING_STOP_BITS_1:
    stop_bits = 1;
  break;
  }

  /* This runs in the tud_task context. The old code parked uart_taskHandle
   * here, which is neither the task that polls these registers every tick
   * (cdc_uart_core2, the highest priority task in the system) nor a safe thing
   * to do: suspending an arbitrary task can strand a mutex holder, and the
   * suspend/resume pair was conditional on tud_cdc_n_connected(), which the host
   * can change in between, leaving the task suspended for good.
   * Hold the hw lock instead - it is the same lock every register access in this
   * file takes - so nothing can touch the block while it is in reset between
   * uart_deinit() and uart_init().
   * Windows opens/reopens the port and re-sends line coding constantly, so this
   * path must never be able to wedge tud_task: the wait is bounded and a failed
   * take simply skips the reconfiguration (the host will set line coding again).
   */
  if (lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS))
  {
    /* Re-check under the lock: a profile switch can have unbound this instance
     * since the unlocked isUartInitialised test in tud_cdc_line_coding_cb.
     * uart_deinit(NULL) would resolve to uart0 and reset an unrelated block. */
    if (cdc_uart_hw_live(uart))
    {
      uart_deinit(uart->uart);
      uart_init(uart->uart, line_coding->bit_rate);
      uart_set_format(uart->uart, data_bits, stop_bits, parity);
    }

    unlock_hw_uart();
  }
}


/* Suspending a task that holds the hw lock strands the lock: the holder can no
 * longer run to release it, so every other UART user would block until it is
 * resumed. Hold the lock across the suspend - the target cannot then be inside a
 * locked region. If the lock cannot be taken in time we still suspend (that is
 * the caller's contract), and every waiter is bounded, so the worst case is a
 * degraded UART, never a wedged tud_task. */
void cdc_uart_suspend_thread(struct CDCUartInstance_t* uart)
{
  if (uart->uart_taskHandle == NULL)  // can be cleared by tud_unmount_cb
    return;

  bool locked = lock_hw_uart(HW_UART_LOCK_TIMEOUT_TICKS);
  vTaskSuspend(uart->uart_taskHandle);
  if (locked)
    unlock_hw_uart();
}


void cdc_uart_line_state_cb(struct CDCUartInstance_t* uart, uint8_t itf, bool dtr, bool rts)
{
#ifdef PROBE_UART_RTS
  gpio_put(uart->rtsPin, !rts);
#endif
#ifdef PROBE_UART_DTR
  gpio_put(PROBE_UART_DTR, !dtr);
#endif

  /* CDC drivers use linestate as a bodge to activate/deactivate the interface.
   * Resume our UART polling on activate, stop on deactivate */
  if (uart->uart_taskHandle == NULL)  // can be cleared by tud_unmount_cb
    return;
  if (!dtr) {
    cdc_uart_suspend_thread(uart);
#ifdef PROBE_UART_RX_LED
    gpio_put(PROBE_UART_RX_LED, 0);
    rx_led_debounce = 0;
#endif
#ifdef PROBE_UART_TX_LED
    gpio_put(PROBE_UART_TX_LED, 0);
    tx_led_debounce = 0;
#endif
  } else
    vTaskResume(uart->uart_taskHandle);
}

