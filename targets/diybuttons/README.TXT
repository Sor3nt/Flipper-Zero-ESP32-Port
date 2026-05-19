//This file contains working input handler for 6 buttons, you will need to wire each pin through a momentary tactile button to ground from the chosen gpio pin. No pull up resistor is needed. Only momemntary tactile buttons will work, not switches. You can also use a 5 way joystick plus a back button, however it must be a 5 way pull low, not a 3 pin x/y joystick (like on an xbox controller), you can also use small capacitive touch sensores, anything that temporarily pulls to ground upon input. 

This is an example declaration needed in chosen boards.h file, add these there and just drop this input handler inplace in your chosen target folder and you have buttons.  
/* ---- Hardware Button Pins ---- */
#define BOARD_PIN_BTN_UP        41
#define BOARD_PIN_BTN_DOWN      40
#define BOARD_PIN_BTN_LEFT      38
#define BOARD_PIN_BTN_RIGHT     39
#define BOARD_PIN_BTN_OK        0
#define BOARD_PIN_BTN_BACK      4
#define BOARD_PIN_BUTTON_BOOT   0  
