#pragma once
#include "wardriver_types.h"
#include "wardriver_csv.h"
#include <storage/storage.h>
typedef struct WardriverLog WardriverLog;
typedef void (*WardriverLogProgress)(void* context,const char* stage);
WardriverLog* wardriver_log_open(uint32_t timestamp, uint32_t session);
bool wardriver_log_record(WardriverLog* log, const WardriverNetwork* network);
bool wardriver_log_flush(WardriverLog* log);
bool wardriver_log_close(WardriverLog* log,WardriverLogProgress progress,void* context);
bool wardriver_log_ok(const WardriverLog* log);
