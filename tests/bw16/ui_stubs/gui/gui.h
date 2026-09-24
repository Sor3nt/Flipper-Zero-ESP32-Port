#pragma once
#include <u8g2.h>
#include <input/input.h>
typedef u8g2_t Canvas;
typedef struct Gui Gui;
typedef struct ViewPort ViewPort;
typedef enum { FontPrimary,FontSecondary } Font;
typedef enum { GuiLayerFullscreen } GuiLayer;
void canvas_clear(Canvas* canvas);
void canvas_set_font(Canvas* canvas,Font font);
void canvas_draw_str(Canvas* canvas,unsigned x,unsigned y,const char* text);
ViewPort* view_port_alloc(void);
void view_port_free(ViewPort* view);
void view_port_draw_callback_set(ViewPort* view,void (*callback)(Canvas*,void*),void* context);
void view_port_input_callback_set(ViewPort* view,void (*callback)(InputEvent*,void*),void* context);
void view_port_update(ViewPort* view);
void gui_add_view_port(Gui* gui,ViewPort* view,GuiLayer layer);
void gui_remove_view_port(Gui* gui,ViewPort* view);
