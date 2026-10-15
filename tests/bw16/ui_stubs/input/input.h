#pragma once
typedef enum { InputKeyUp,InputKeyDown,InputKeyLeft,InputKeyRight,InputKeyOk,InputKeyBack } InputKey;
typedef enum { InputTypePress,InputTypeRelease,InputTypeShort,InputTypeLong,InputTypeRepeat } InputType;
typedef struct { InputKey key; InputType type; } InputEvent;
