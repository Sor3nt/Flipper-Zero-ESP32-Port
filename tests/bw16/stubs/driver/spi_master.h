#pragma once
#include <esp_err.h>
#define SPI2_HOST 1
#define portMAX_DELAY 0xffffffffU
#define SPI_TRANS_USE_TXDATA 1
#define SPI_TRANS_USE_RXDATA 2
typedef void* spi_device_handle_t;
typedef struct { int clock_speed_hz,mode,spics_io_num,queue_size; } spi_device_interface_config_t;
typedef struct { unsigned flags,length; uint8_t tx_data[4],rx_data[4]; } spi_transaction_t;
esp_err_t spi_bus_add_device(int host,const spi_device_interface_config_t* cfg,spi_device_handle_t* device);
esp_err_t spi_bus_remove_device(spi_device_handle_t device);
esp_err_t spi_device_acquire_bus(spi_device_handle_t device,unsigned wait);
void spi_device_release_bus(spi_device_handle_t device);
esp_err_t spi_device_polling_transmit(spi_device_handle_t device,spi_transaction_t* t);
