if(FLIPPER_BOARD STREQUAL "thormini")
    set(VIEW_INPUT_SRC "view_dispatcher_thormini.c")
else()
    set(VIEW_INPUT_SRC "view_dispatcher.c")
endif()

if(FLIPPER_BOARD STREQUAL "thormini")
    set(TEXT_INPUT_SRC "modules/text_input_thormini.c")
else()
    set(TEXT_INPUT_SRC "modules/text_input.c")
endif()


idf_component_register(
    SRCS
        "gui.c"
        "canvas.c"
        "icon.c"
        "icon_animation.c"
        "elements.c"
        "view.c"
        "view_port.c"
        "view_holder.c"
        ${VIEW_INPUT_SRC}
        "view_stack.c"
        "scene_manager.c"
        "modules/button_menu.c"
        "modules/button_panel.c"
        "modules/byte_input.c"
        "modules/date_time_input.c"
        "modules/dialog_ex.c"
        "modules/empty_screen.c"
        "modules/file_browser.c"
        "modules/file_browser_worker.c"
        "modules/loading.c"
        "modules/menu.c"
        "modules/number_input.c"
        "modules/popup.c"
        "modules/submenu.c"
        "modules/text_box.c"
        ${TEXT_INPUT_SRC}
        "modules/validators.c"
        "modules/variable_item_list.c"
        "modules/widget.c"
        "modules/widget_elements/widget_element_button.c"
        "modules/widget_elements/widget_element_circle.c"
        "modules/widget_elements/widget_element_icon.c"
        "modules/widget_elements/widget_element_line.c"
        "modules/widget_elements/widget_element_rect.c"
        "modules/widget_elements/widget_element_string.c"
        "modules/widget_elements/widget_element_string_multiline.c"
        "modules/widget_elements/widget_element_text_box.c"
        "modules/widget_elements/widget_element_text_scroll.c"
    INCLUDE_DIRS "." "modules" "modules/widget_elements"
    REQUIRES furi furi_hal u8g2 toolbox input notification assets mlib datetime storage
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32_HAS_STORAGE=1)
