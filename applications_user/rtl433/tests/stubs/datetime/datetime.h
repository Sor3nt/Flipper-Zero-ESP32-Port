#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint8_t hour,minute,second,day,month;uint16_t year;uint8_t weekday;}DateTime;
uint32_t datetime_datetime_to_timestamp(DateTime *date);
