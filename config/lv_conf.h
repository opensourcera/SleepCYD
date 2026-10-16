/* LVGL 9 configuration for the Cheap Yellow Display firmware (everything else takes lv_conf_internal.h defaults) */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* widgets used */
#define LV_USE_LABEL 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_USE_QRCODE 1   /* SleepCYD: QR code with the report address */
#define LV_USE_BUTTON 1
#define LV_USE_IMAGE 1
#define LV_USE_LIST 1
#define LV_USE_KEYBOARD 1
#define LV_USE_TEXTAREA 1
#define LV_USE_DROPDOWN 1
#define LV_USE_SWITCH 1
#define LV_USE_BUTTONMATRIX 1

/* unused: save flash */
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 1
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 1
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_ANIMIMG 0
#define LV_USE_LOTTIE 0

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

#endif
