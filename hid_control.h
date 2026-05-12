#pragma once
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h>

// Globálne HID objekty
USBHIDMouse   hidMouse;
USBHIDKeyboard hidKeyboard;

// Inicializácia USB HID – volaj v setup() PRED WiFi
void hid_begin() {
    hidMouse.begin();
    hidKeyboard.begin();
    USB.begin();
}

// ── Myš ──────────────────────────────────────────────────────────────────────

inline void mouse_move(int8_t dx, int8_t dy) {
    hidMouse.move(dx, dy, 0);
}

inline void mouse_scroll(int8_t delta) {
    hidMouse.move(0, 0, delta);
}

inline void mouse_click(uint8_t btn = MOUSE_LEFT) {
    hidMouse.click(btn);
}

inline void mouse_press(uint8_t btn = MOUSE_LEFT) {
    hidMouse.press(btn);
}

inline void mouse_release(uint8_t btn = MOUSE_LEFT) {
    hidMouse.release(btn);
}

// ── Klávesnica ───────────────────────────────────────────────────────────────

inline void kb_print(const char* text) {
    hidKeyboard.print(text);
}

inline void kb_println(const char* text) {
    hidKeyboard.println(text);
}

// Stlač a pusti jeden kláves (napr. KEY_RETURN, KEY_LEFT_CTRL, ...)
inline void kb_tap(uint8_t key) {
    hidKeyboard.press(key);
    delay(30);
    hidKeyboard.release(key);
}

// Kombinácia kláves: drž modifier (napr. KEY_LEFT_CTRL) a stlač key
inline void kb_combo(uint8_t modifier, uint8_t key) {
    hidKeyboard.press(modifier);
    delay(10);
    hidKeyboard.press(key);
    delay(30);
    hidKeyboard.releaseAll();
}
