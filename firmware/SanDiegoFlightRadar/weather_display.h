#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <lvgl.h>
namespace weather {
using FetchJSON=int (*)(const String&,JsonDocument&,uint32_t&);
void init(lv_obj_t *flightScreen,double *latitude,double *longitude,FetchJSON fetch);
void show(bool visible);
void showWind(bool weekly=false);
void zoom(int delta);
void layer(bool infrared);
void info();
void tick();
void service();
void diagnostics();
}
