#pragma once
typedef struct ViewPort ViewPort;
typedef enum { InputKeyUp,InputKeyDown,InputKeyLeft,InputKeyRight,InputKeyOk,InputKeyBack } InputKey;
typedef enum { InputTypePress,InputTypeRelease,InputTypeShort,InputTypeLong,InputTypeRepeat } InputType;
typedef struct { InputKey key; InputType type; } InputEvent;
void view_port_update(ViewPort* view);
