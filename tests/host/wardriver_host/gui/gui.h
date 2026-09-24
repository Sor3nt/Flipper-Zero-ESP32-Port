#pragma once
#include <stddef.h>
#include <stdint.h>
#include <u8g2.h>
typedef struct Gui Gui;
typedef u8g2_t Canvas;
typedef enum { ColorWhite,ColorBlack } Color;
typedef enum { FontPrimary,FontSecondary } Font;
typedef enum { AlignLeft,AlignRight,AlignTop,AlignBottom,AlignCenter } Align;
void canvas_clear(Canvas* canvas);
void canvas_set_font(Canvas* canvas,Font font);
void canvas_set_color(Canvas* canvas,Color color);
void canvas_draw_rbox(Canvas* canvas,unsigned x,unsigned y,unsigned w,unsigned h,unsigned r);
void canvas_draw_str(Canvas* canvas,unsigned x,unsigned y,const char* text);
void canvas_draw_str_aligned(Canvas* canvas,unsigned x,unsigned y,Align horizontal,Align vertical,const char* text);
void canvas_draw_line(Canvas* canvas,unsigned x,unsigned y,unsigned x2,unsigned y2);
size_t canvas_string_width(Canvas* canvas,const char* text);

void canvas_draw_rframe(Canvas* canvas,unsigned x,unsigned y,unsigned w,unsigned h,unsigned r);
