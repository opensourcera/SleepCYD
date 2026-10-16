// One BLE notification as the NimBLE task hands it to the SD writer through a FreeRTOS queue (packed, 10-byte header
// + data). In its own header so the Arduino prototype generator sees the type before the functions that use it.
#pragma once
#include <stdint.h>
struct __attribute__((packed)) Rec { uint32_t ms; uint16_t handle; uint16_t len; uint8_t kind, pad; uint8_t data[512]; };
