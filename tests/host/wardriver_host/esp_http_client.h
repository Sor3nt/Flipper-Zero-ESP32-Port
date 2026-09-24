#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
enum { HTTP_METHOD_POST,HTTP_TRANSPORT_OVER_SSL,HTTP_AUTH_TYPE_BASIC };
typedef struct Http* esp_http_client_handle_t;
typedef struct {
    const char *url,*username,*password,*user_agent;
    int method,transport_type,timeout_ms,buffer_size,buffer_size_tx,auth_type;
    esp_err_t (*crt_bundle_attach)(void*);
    bool skip_cert_common_name_check,disable_auto_redirect,keep_alive_enable;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*);
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);
int esp_http_client_write(esp_http_client_handle_t,const char*,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,int);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
