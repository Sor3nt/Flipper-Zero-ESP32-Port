#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef void* QueueHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_SUPPORTED 2
#define UART_NUM_1 1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
#define pdMS_TO_TICKS(n) (n)
typedef struct { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
bool uart_is_driver_installed(int n);
esp_err_t uart_driver_install(int n,int rx,int tx,int count,QueueHandle_t* q,int flags);
esp_err_t uart_driver_delete(int n);
esp_err_t uart_param_config(int n,const uart_config_t* cfg);
esp_err_t uart_set_pin(int n,int tx,int rx,int rts,int cts);
esp_err_t uart_flush_input(int n);
int uart_write_bytes(int n,const void* p,size_t len);
esp_err_t uart_wait_tx_done(int n,unsigned timeout);
