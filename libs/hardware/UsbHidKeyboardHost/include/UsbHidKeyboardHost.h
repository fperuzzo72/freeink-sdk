#pragma once

// USB HID Host for wired keyboards, connected via a USB-OTG (host) adapter on
// the same USB-C port already used for CDC/flashing. Talks directly to
// ESP-IDF's low-level usb_host driver (usb/usb_host.h, bundled with the
// arduino-esp32 core -- no extra managed component needed) rather than any
// higher-level HID class driver, so this is the whole HID Boot Keyboard
// protocol in one small module: enumerate, claim the interface, force Boot
// Protocol, and read 8-byte boot reports off the interrupt IN endpoint.
//
// OPT-IN per board via FREEINK_CAP_USB_HID_KBD_HOST (see BoardConfig.h) --
// requires the ESP32-S2/S3's native USB-OTG peripheral, and claims the same
// shared USB PHY the board's CDC console uses: the two are mutually
// exclusive on real hardware (there is exactly one USB-C port), so a board
// enabling this should also turn off the native CDC's auto-begin
// (-DARDUINO_USB_CDC_ON_BOOT=0) to leave the PHY free for usb_host_install().
// Flashing over the same port keeps working regardless -- the ROM bootloader
// re-claims the PHY itself on every reset, before any app code (including
// this library) runs. When the capability is off this compiles to a trivial
// stub and links no USB Host code.
//
// Output is deliberately NOT a translated character stream: every event is a
// raw USB HID [keyCode, modifiers, pressed] triple -- the exact shape this
// project's own enqueueKeyEvent() already expects, and modifiers is the
// literal USB HID Boot Report modifier byte (bit-for-bit config.h's own
// MOD_CTRL_LEFT/MOD_SHIFT_LEFT/... masks). A physical keyboard becomes just
// another producer for the same queue the on-screen keyboard already feeds,
// so US-International dead-key composition, Ctrl+C break, and every other
// downstream consumer work unchanged -- no separate translation layer.
//
// Modifier keys (Ctrl/Shift/Alt/GUI) never appear as their own keyCode: the
// HID Boot Report encodes them solely in the modifier byte, never in the
// 6-key array, so this mirrors exactly how the on-screen keyboard's own
// Shift/Ctrl/Alt keys work -- they arm state but never enqueue an event of
// their own; only a following content key carries the modifier along.
//
// No auto-repeat yet (matches the on-screen keyboard's own current
// behavior, which also has none) -- a held key sends exactly one press
// event, not a stream. Worth adding once basic input is confirmed working
// on hardware.

#include <BoardConfig.h>

#if FREEINK_CAP_USB_HID_KBD_HOST

#include <cstdint>

namespace freeink {

class UsbHidKeyboardHost {
 public:
  static UsbHidKeyboardHost& getInstance();

  // Installs the USB Host Library, registers a client, and spawns the two
  // background FreeRTOS tasks that service it (one for
  // usb_host_lib_handle_events(), one for usb_host_client_handle_events()).
  // Returns false if already begun or if the USB Host Library failed to
  // install. Safe to call once from setup(); there is no end() -- this is
  // meant to run for the device's whole uptime, same as InputManager.
  bool begin();

  // True while a HID Boot Keyboard interface is currently claimed and its
  // interrupt IN transfer is live. False before any keyboard is plugged in,
  // and again after it's unplugged.
  bool connected() const;

  // Pops the next queued key-press event (see file header for the [keyCode,
  // modifiers, pressed] contract). Returns false if the queue is empty.
  // Safe to call from the Arduino main loop -- events are produced on a
  // background task and handed off through a FreeRTOS queue, never called
  // back into directly.
  bool popKey(uint8_t& keyCode, uint8_t& modifiers, bool& pressed);

  // Diagnostics -- there is no serial console while this capability is on
  // (see the port-sharing note above), so these exist for a caller to print
  // to the e-ink screen itself and see which stage of enumeration a plugged-
  // in keyboard actually reached. Plain reads of small ints/bools written
  // from the USB background task; not a synchronization primitive, just
  // good enough for a periodically-polled status line.
  bool beganOk() const;         // usb_host_install() + client_register() succeeded
  uint32_t devicesSeen() const; // USB_HOST_CLIENT_EVENT_NEW_DEV count, any device
  uint32_t rejectedCount() const;  // of those, how many weren't a usable HID boot keyboard
  uint32_t reportsReceived() const;  // completed interrupt-IN transfers from a claimed keyboard
  uint8_t lastKeyCode() const;   // most recent raw report's first keycode byte (0 = none yet)
  uint8_t lastModifiers() const; // most recent raw report's modifier byte
};

}  // namespace freeink

#else  // !FREEINK_CAP_USB_HID_KBD_HOST -- stub, no USB Host code linked

namespace freeink {

class UsbHidKeyboardHost {
 public:
  static UsbHidKeyboardHost& getInstance();
  bool begin() { return false; }
  bool connected() const { return false; }
  bool popKey(uint8_t&, uint8_t&, bool&) { return false; }
  bool beganOk() const { return false; }
  uint32_t devicesSeen() const { return 0; }
  uint32_t rejectedCount() const { return 0; }
  uint32_t reportsReceived() const { return 0; }
  uint8_t lastKeyCode() const { return 0; }
  uint8_t lastModifiers() const { return 0; }
};

}  // namespace freeink

#endif  // FREEINK_CAP_USB_HID_KBD_HOST

// Mirrors SdMan/BleHid's own alias pattern elsewhere in the SDK.
#define UsbKbd ::freeink::UsbHidKeyboardHost::getInstance()
