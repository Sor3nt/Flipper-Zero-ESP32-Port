idf_component_register(
    SRCS "notification.c" "notification_messages.c" "notification_messages_notes.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal input
)
