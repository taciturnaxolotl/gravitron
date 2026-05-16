#include "uart_bridge.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "uart";
static uart_rx_callback_t g_rx_callback = NULL;
static QueueHandle_t g_rx_queue = NULL;

#define UART_NUM          UART_NUM_2
#define UART_RX_QUEUE_LEN 16

static void uart_rx_task(void *arg)
{
    uint8_t buf[UART_RX_BUF_SIZE];
    char line[UART_RX_BUF_SIZE];
    int line_idx = 0;

    while (1) {
        int len = uart_read_bytes(UART_NUM, buf, sizeof(buf) - 1,
                                  pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        for (int i = 0; i < len; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_idx > 0) {
                    line[line_idx] = '\0';
                    if (g_rx_callback) {
                        g_rx_callback(line, line_idx);
                    }
                    line_idx = 0;
                }
            } else if (line_idx < (int)sizeof(line) - 1) {
                line[line_idx++] = c;
            }
        }
    }
}

void uart_bridge_init(int rx_pin, int tx_pin, int baud)
{
    uart_config_t uart_config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_RX_BUF_SIZE * 2,
                                        UART_RX_BUF_SIZE * 2, UART_RX_QUEUE_LEN,
                                        &g_rx_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "UART init: rx=%d tx=%d baud=%d", rx_pin, tx_pin, baud);
}

void uart_bridge_set_rx_callback(uart_rx_callback_t cb)
{
    g_rx_callback = cb;
}

bool uart_bridge_send(const char *data, int len)
{
    int sent = uart_write_bytes(UART_NUM, data, len);
    if (sent < 0) {
        ESP_LOGE(TAG, "UART send failed");
        return false;
    }
    // append newline as frame delimiter for Arduino
    uart_write_bytes(UART_NUM, "\n", 1);
    return true;
}

void uart_bridge_flush_rx(void)
{
    uart_flush_input(UART_NUM);
}
