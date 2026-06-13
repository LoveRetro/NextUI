/*
 * On-Screen Keyboard for minarch
 * Ported from settings/keyboardprompt.cpp
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

// Show on-screen keyboard and return entered text
// title: Title to display above the keyboard
// Returns malloc'd string with input, or NULL if cancelled
// Caller must free() the returned string
char* Keyboard_show(const char* title);

// Convenience function for WiFi password entry
char* Keyboard_getPassword(void);

#endif // KEYBOARD_H
