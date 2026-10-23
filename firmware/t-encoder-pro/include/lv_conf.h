// Minimal LVGL 9.2 config for the knob. Anything not set here uses LVGL's default.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (96 * 1024U)
#define LV_DEF_REFR_PERIOD 20
#define LV_USE_LOG 0
#define LV_USE_OS LV_OS_NONE

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_THEME_DEFAULT 0

#endif // LV_CONF_H
