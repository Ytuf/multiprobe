/*
 * The MIT License (MIT)
 *
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

#ifndef BOARD_MULTIPROBE_H_
#define BOARD_MULTIPROBE_H_

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// ---------------------------------------------------------------------------
// FreeWili pin map (RP2350B)
// ---------------------------------------------------------------------------
// External PROM EEPROMs on I2C
#define PROM_I2C_SDA_PIN     0       // I2C0 SDA -> PROM
#define PROM_I2C_SCL_PIN     1       // I2C0 SCL -> PROM
#define PROM_I2C_INSTANCE    i2c0
#define PROM1_I2C_SDA_PIN    18      // I2C1 SDA -> PROM1
#define PROM1_I2C_SCL_PIN    19      // I2C1 SCL -> PROM1
#define PROM1_I2C_INSTANCE   i2c1

// Status / indicator LEDs
#define BOARD_LED_PIN        2       // Driven high at boot (steady on)
#define SWD_READY_LED_PIN    15      // Green: any SWD probe connected (initted)
#define SWD_ACT_LED_PIN      16      // Yellow: SWD transaction recently observed

// PIC ICSP master clear pull line
// Configured as input with internal pull-up: idles high, not actively driven.
// TODO: revisit if/when MCLR_PULL needs to be driven (e.g., to assert reset).
#define PIC_MCLR_PULL_PIN    3

// TODO: GPIO4/5 are wired to PIC16 ICSPDAT/ICSPCLK. They are currently
//       configured as UART1 (UART_CONFIG0 / "PIC" profile). Verify that the
//       PIC firmware actually exposes a UART on these pins -- if it's pure
//       ICSP we'll need a different driver, not the CDC-UART path.

// ---------------------------------------------------------------------------

// UART config - FreeWili: GPIO4/5 is PIC UART (TBD - see note above), keep UART1 for that; disable others to avoid SWD pin conflicts
#define UART_CONFIG0 { .index=0, .name="PIC", .txPin=4, .rxPin=5, .hwUartId=1, .baud=115200, .activityLedPin=-1 }           // PIC16 UART/ICSP (GPIO4/5) - confirm
#define UART_CONFIG1 { .index=1, .name="UNUSED1", .txPin=-1, .rxPin=-1, .hwUartId=0, .baud=115200, .activityLedPin=-1 }     // Disabled - pins used for SWD
#define UART_CONFIG2 { .index=2, .name="UNUSED2", .txPin=-1, .rxPin=-1, .hwUartId=1, .baud=115200, .activityLedPin=-1 }     // Disabled - pins used for SWD
#define UART_CONFIG3 { .index=3, .name="UNUSED3", .txPin=-1, .rxPin=-1, .hwUartId=0, .baud=115200, .activityLedPin=-1 }     // Disabled

// SWD probe config - FreeWili RP2040 debug processor pin assignments
#define PROBE_CONFIG0 {.name="DISPLAY", .pio=pio0, .sm=0, .clkPin=10, .dioPin=11, .activityLedPin=-1}                       // Display RP2350B
#define PROBE_CONFIG1 {.name="MAIN",    .pio=pio0, .sm=1, .clkPin=12, .dioPin=13, .activityLedPin=-1}                       // Main RP2350B (GPIO14 tied to GPIO13 externally; left at default input/no-pull)
#define PROBE_CONFIG2 {.name="WIO-E5",  .pio=pio1, .sm=0, .clkPin=9,  .dioPin=8,  .activityLedPin=-1}                       // STM32WLE5JC
#define PROBE_CONFIG3 {.name="UNUSED",  .pio=pio1, .sm=1, .clkPin=-1, .dioPin=-1, .activityLedPin=-1}                       // Disabled

// #define PROBE_USB_CONNECTED_LED -1  // FreeWili: no dedicated USB LED

#define PROBE_PRODUCT_STRING "FreeWili Debug Probe (CMSIS-DAP)"

#endif
