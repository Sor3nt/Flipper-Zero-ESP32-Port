#include "../../applications/services/rpc/rpc_app.h"

void rpc_system_app_set_callback(
    RpcAppSystem* rpc_app,
    RpcAppSystemCallback callback,
    void* context) {
    (void)rpc_app;
    (void)callback;
    (void)context;
}

void rpc_system_app_send_started(RpcAppSystem* rpc_app) {
    (void)rpc_app;
}

void rpc_system_app_send_exited(RpcAppSystem* rpc_app) {
    (void)rpc_app;
}

void rpc_system_app_confirm(RpcAppSystem* rpc_app, bool result) {
    (void)rpc_app;
    (void)result;
}

void rpc_system_app_set_error_code(RpcAppSystem* rpc_app, uint32_t error_code) {
    (void)rpc_app;
    (void)error_code;
}

void rpc_system_app_set_error_text(RpcAppSystem* rpc_app, const char* error_text) {
    (void)rpc_app;
    (void)error_text;
}

void rpc_system_app_error_reset(RpcAppSystem* rpc_app) {
    (void)rpc_app;
}

void rpc_system_app_exchange_data(RpcAppSystem* rpc_app, const uint8_t* data, size_t data_size) {
    (void)rpc_app;
    (void)data;
    (void)data_size;
}
