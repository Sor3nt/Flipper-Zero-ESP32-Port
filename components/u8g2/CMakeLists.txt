idf_component_register(
    SRCS
        "u8g2_bitmap.c"
        "u8g2_box.c"
        "u8g2_buffer.c"
        "u8g2_circle.c"
        "u8g2_d_memory.c"
        "u8g2_font.c"
        "u8g2_fonts.c"
        "u8g2_glue.c"
        "u8g2_hvline.c"
        "u8g2_intersection.c"
        "u8g2_line.c"
        "u8g2_ll_hvline.c"
        "u8g2_setup.c"
        "u8x8_8x8.c"
        "u8x8_byte.c"
        "u8x8_cad.c"
        "u8x8_display.c"
        "u8x8_gpio.c"
        "u8x8_setup.c"
    INCLUDE_DIRS "."
    REQUIRES furi
)
