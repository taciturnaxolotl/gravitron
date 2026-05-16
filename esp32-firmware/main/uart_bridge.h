#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_RX_BUF_SIZE  512
#define UART_TX_BUF_SIZE  512

typedef void (*uart_rx_callback_t)(const char *json_str, int len);

void uart_bridge_init(int rx_pin, int tx_pin, int baud);
void uart_bridge_set_rx_callback(uart_rx_callback_t cb);
bool uart_bridge_send(const char *data, int len);
void uart_bridge_flush_rx(void);

#ifdef __cplusplus
}
#endif

#endif
