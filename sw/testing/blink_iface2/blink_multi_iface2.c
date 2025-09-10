#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

// uart 1
#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1

#define BAUD_RATE 115200
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY    UART_PARITY_NONE
#define POSTIFX "[2]"

bool g_EchoMode = false;

void on_uart_rx() 
{
    while (uart_is_readable(UART_ID)) 
    {
        uint8_t ch = uart_getc(UART_ID);
        
        g_EchoMode = true;
        
        if (uart_is_writable(UART_ID)) 
        {                    
            uart_putc(UART_ID, ch);
        }
    }
}


int main()
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);    
    gpio_put(PICO_DEFAULT_LED_PIN,1);
    sleep_ms(200);
    gpio_put(PICO_DEFAULT_LED_PIN,0);

    uart_init(UART_ID, BAUD_RATE);

    gpio_set_function(UART_TX_PIN, UART_FUNCSEL_NUM(UART_ID, UART_TX_PIN));
    gpio_set_function(UART_RX_PIN, UART_FUNCSEL_NUM(UART_ID, UART_RX_PIN));

    int __unused actual = uart_set_baudrate(UART_ID, BAUD_RATE);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);
    uart_set_fifo_enabled(UART_ID, false);

    int UART_IRQ = UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
    irq_set_enabled(UART_IRQ, true);

    uart_set_irq_enables(UART_ID, true, false);
    
    uart_puts(UART_ID, "blink_multi_iface init " POSTIFX " \n");

    while (true) 
    {
        if(!g_EchoMode)
            uart_puts(UART_ID, "blink_multi_iface" POSTIFX "!\n");

        gpio_put(PICO_DEFAULT_LED_PIN,0);
        sleep_ms(1000);
        gpio_put(PICO_DEFAULT_LED_PIN,1);
        sleep_ms(1000);
    }
}