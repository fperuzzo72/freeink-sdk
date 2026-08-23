// FreeInk SDK — USB HID Keyboard Host implementation.
//
// The real usb_host driver path compiles only under FREEINK_CAP_USB_HID_KBD_HOST;
// the #else branch (in the header) links stub bodies and references no USB
// Host code. Flow: usb_host_install() -> register a client -> two background
// tasks service the library and the client -> on NEW_DEV, check whether the
// device exposes a HID Boot Keyboard interface (class 3 / subclass 1 / protocol
// 1) -> claim it -> force Boot Protocol -> submit a persistent interrupt IN
// transfer -> each completed transfer is an 8-byte boot report, diffed against
// the previous one to find newly-pressed keys, which get pushed into a queue
// the Arduino main loop drains via popKey().

#include "UsbHidKeyboardHost.h"

namespace freeink {

UsbHidKeyboardHost& UsbHidKeyboardHost::getInstance() {
  static UsbHidKeyboardHost instance;
  return instance;
}

}  // namespace freeink

#if FREEINK_CAP_USB_HID_KBD_HOST

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include <cstring>

namespace freeink {
namespace {

// HID Boot Interface constants (USB HID 1.11, section 4.2/4.3). Not in
// usb_types_ch9.h -- those are class-specific, not part of the standard
// chapter-9 descriptor set the header covers.
constexpr uint8_t kHidSubclassBoot = 1;
constexpr uint8_t kHidProtocolKeyboard = 1;
constexpr uint8_t kHidReqSetIdle = 0x0A;
constexpr uint8_t kHidReqSetProtocol = 0x0B;
constexpr uint8_t kHidProtocolBoot = 0;
// bmRequestType: host-to-device, class, interface (USB 2.0 table 9-2).
constexpr uint8_t kHidClassInterfaceOut = 0x21;

struct KeyMsg {
  uint8_t keyCode;
  uint8_t modifiers;
};

usb_host_client_handle_t gClientHdl = nullptr;
usb_device_handle_t gDevHdl = nullptr;
uint8_t gInterfaceNumber = 0xFF;  // 0xFF = none claimed
uint8_t gEndpointAddr = 0;
usb_transfer_t* gIntrTransfer = nullptr;
uint8_t gPrevReport[8] = {0};
volatile bool gConnected = false;
QueueHandle_t gKeyQueue = nullptr;
SemaphoreHandle_t gCtrlDone = nullptr;

// Diagnostics -- see the header's doc comment on why these exist at all
// (no serial console while this capability is on). Plain globals, written
// only from the USB client task, read only from the Arduino loop's status
// line; good enough for that, not meant as a synchronization primitive.
volatile bool gBeganOk = false;
volatile uint32_t gDevicesSeen = 0;
volatile uint32_t gRejectedCount = 0;
volatile uint32_t gReportsReceived = 0;
volatile uint8_t gLastKeyCode = 0;
volatile uint8_t gLastModifiers = 0;

// Submits a HID class control request (SET_PROTOCOL/SET_IDLE) with no data
// stage and blocks (briefly -- we're on the dedicated client task, not an
// ISR, so this is safe) until it completes or times out. Both requests only
// run once, right after claiming the interface, so a short-lived transfer
// allocated per call is simpler than keeping one around for reuse.
void onControlDone(usb_transfer_t* /*transfer*/) { xSemaphoreGive(gCtrlDone); }

void submitClassControl(uint8_t bRequest, uint16_t wValue, uint16_t wIndex) {
  usb_transfer_t* xfer;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &xfer) != ESP_OK) return;
  auto* setup = reinterpret_cast<usb_setup_packet_t*>(xfer->data_buffer);
  setup->bmRequestType = kHidClassInterfaceOut;
  setup->bRequest = bRequest;
  setup->wValue = wValue;
  setup->wIndex = wIndex;
  setup->wLength = 0;
  xfer->device_handle = gDevHdl;
  xfer->bEndpointAddress = 0;
  xfer->num_bytes = sizeof(usb_setup_packet_t);
  xfer->callback = onControlDone;
  xfer->context = nullptr;
  xSemaphoreTake(gCtrlDone, 0);  // drop any stale signal before submitting
  if (usb_host_transfer_submit_control(gClientHdl, xfer) == ESP_OK) {
    xSemaphoreTake(gCtrlDone, pdMS_TO_TICKS(200));
  }
  usb_host_transfer_free(xfer);
}

void onReport(usb_transfer_t* xfer) {
  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes >= 8) {
    gReportsReceived = gReportsReceived + 1;
    const uint8_t* report = xfer->data_buffer;
    const uint8_t modifiers = report[0];
    gLastModifiers = modifiers;
    gLastKeyCode = report[2];
    // report[2..7] is the 6-key rollover array. 0 means "no key here"; 1
    // means "too many keys/rollover error" -- neither is a real keycode.
    // Only a keycode that's newly present (wasn't in the previous report)
    // becomes a press event; this project's own queue never wants release
    // events (processAllInput() only acts on event.pressed, and the
    // on-screen keyboard never sends pressed=false either -- see this
    // library's header).
    for (int i = 2; i < 8; i++) {
      const uint8_t code = report[i];
      if (code < 4) continue;
      bool wasDown = false;
      for (int j = 2; j < 8; j++) {
        if (gPrevReport[j] == code) { wasDown = true; break; }
      }
      if (!wasDown) {
        KeyMsg msg{code, modifiers};
        xQueueSend(gKeyQueue, &msg, 0);
      }
    }
    memcpy(gPrevReport, report, 8);
  }

  // NO_DEVICE means the keyboard is already gone; the client's DEV_GONE
  // event handles teardown (including freeing this transfer), so don't
  // resubmit into a device handle that's about to be closed out from under
  // it. Any other status (a transient stall/timeout/short read) just
  // re-arms for the next report rather than going silently deaf.
  if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
    usb_host_transfer_submit(xfer);
  }
}

void handleNewDevice(uint8_t addr) {
  gDevicesSeen = gDevicesSeen + 1;
  if (gDevHdl != nullptr) return;  // one keyboard at a time

  usb_device_handle_t dev;
  if (usb_host_device_open(gClientHdl, addr, &dev) != ESP_OK) {
    gRejectedCount = gRejectedCount + 1;
    return;
  }

  const usb_config_desc_t* cfg;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
    gRejectedCount = gRejectedCount + 1;
    usb_host_device_close(gClientHdl, dev);
    return;
  }

  const usb_intf_desc_t* intf = nullptr;
  int intfNum = -1;
  for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
    int offset = 0;
    const usb_intf_desc_t* cand = usb_parse_interface_descriptor(cfg, i, 0, &offset);
    if (cand != nullptr && cand->bInterfaceClass == USB_CLASS_HID &&
        cand->bInterfaceSubClass == kHidSubclassBoot && cand->bInterfaceProtocol == kHidProtocolKeyboard) {
      intf = cand;
      intfNum = i;
      break;
    }
  }
  if (intf == nullptr) {
    // Not a boot keyboard (a mouse, or a composite device's other
    // interface) -- leave it alone rather than claiming something we can't
    // usefully talk to.
    gRejectedCount = gRejectedCount + 1;
    usb_host_device_close(gClientHdl, dev);
    return;
  }

  const usb_ep_desc_t* ep = nullptr;
  for (int idx = 0; idx < intf->bNumEndpoints; idx++) {
    int offset = 0;
    const usb_ep_desc_t* cand = usb_parse_endpoint_descriptor_by_index(intf, idx, cfg->wTotalLength, &offset);
    if (cand != nullptr && USB_EP_DESC_GET_XFERTYPE(cand) == USB_TRANSFER_TYPE_INTR &&
        USB_EP_DESC_GET_EP_DIR(cand) == 1) {
      ep = cand;
      break;
    }
  }
  if (ep == nullptr) {
    gRejectedCount = gRejectedCount + 1;
    usb_host_device_close(gClientHdl, dev);
    return;
  }

  if (usb_host_interface_claim(gClientHdl, dev, intfNum, 0) != ESP_OK) {
    gRejectedCount = gRejectedCount + 1;
    usb_host_device_close(gClientHdl, dev);
    return;
  }

  gDevHdl = dev;
  gInterfaceNumber = static_cast<uint8_t>(intfNum);
  gEndpointAddr = ep->bEndpointAddress;
  memset(gPrevReport, 0, sizeof(gPrevReport));

  submitClassControl(kHidReqSetProtocol, kHidProtocolBoot, gInterfaceNumber);
  submitClassControl(kHidReqSetIdle, 0, gInterfaceNumber);

  if (gIntrTransfer == nullptr && usb_host_transfer_alloc(8, 0, &gIntrTransfer) != ESP_OK) {
    gRejectedCount = gRejectedCount + 1;
    usb_host_interface_release(gClientHdl, dev, gInterfaceNumber);
    usb_host_device_close(gClientHdl, dev);
    gDevHdl = nullptr;
    gInterfaceNumber = 0xFF;
    return;
  }
  gIntrTransfer->device_handle = gDevHdl;
  gIntrTransfer->bEndpointAddress = gEndpointAddr;
  gIntrTransfer->num_bytes = 8;
  gIntrTransfer->callback = onReport;
  gIntrTransfer->context = nullptr;
  usb_host_transfer_submit(gIntrTransfer);

  gConnected = true;
}

void handleDeviceGone() {
  if (gDevHdl == nullptr) return;
  gConnected = false;
  // Halt + flush before freeing: the standard teardown sequence for an
  // endpoint that may still have a transfer in flight (Espressif's own
  // hid_host example does the same) -- freeing straight away risks freeing
  // a transfer the Host Library hasn't finished with yet.
  usb_host_endpoint_halt(gDevHdl, gEndpointAddr);
  usb_host_endpoint_flush(gDevHdl, gEndpointAddr);
  if (gIntrTransfer != nullptr) {
    usb_host_transfer_free(gIntrTransfer);
    gIntrTransfer = nullptr;
  }
  usb_host_interface_release(gClientHdl, gDevHdl, gInterfaceNumber);
  usb_host_device_close(gClientHdl, gDevHdl);
  gDevHdl = nullptr;
  gInterfaceNumber = 0xFF;
}

void onClientEvent(const usb_host_client_event_msg_t* event_msg, void* /*arg*/) {
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      handleNewDevice(event_msg->new_dev.address);
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      handleDeviceGone();
      break;
  }
}

// usb_host_lib_handle_events() drives the Host Library's own state machine
// (root port, enumeration, control transfers to unclaimed devices) and must
// be serviced continuously and promptly -- hence its own dedicated task
// rather than a call from the Arduino main loop, which the MicroBASIC
// interpreter can block for arbitrarily long stretches while a program runs.
void usbLibTask(void* /*arg*/) {
  while (true) {
    uint32_t eventFlags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
  }
}

// usb_host_client_handle_events() dispatches this client's NEW_DEV/DEV_GONE
// events and every transfer-completion callback submitted through this
// client (onControlDone, onReport) -- all from this task's own context, safe
// to make further usb_host_* calls from, unlike an ISR.
void usbClientTask(void* /*arg*/) {
  while (true) {
    usb_host_client_handle_events(gClientHdl, portMAX_DELAY);
  }
}

}  // namespace

bool UsbHidKeyboardHost::begin() {
  if (gKeyQueue != nullptr) return false;  // already begun

  gKeyQueue = xQueueCreate(32, sizeof(KeyMsg));
  gCtrlDone = xSemaphoreCreateBinary();
  if (gKeyQueue == nullptr || gCtrlDone == nullptr) return false;

  usb_host_config_t hostConfig = {};
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hostConfig) != ESP_OK) return false;

  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 5;
  clientConfig.async.client_event_callback = onClientEvent;
  clientConfig.async.callback_arg = nullptr;
  if (usb_host_client_register(&clientConfig, &gClientHdl) != ESP_OK) {
    usb_host_uninstall();
    return false;
  }

  // Pinned to core 0: the Arduino main loop (core 1 by default) can block
  // for a long time inside a running BASIC program, and USB Host servicing
  // can't afford to wait on it.
  xTaskCreatePinnedToCore(usbLibTask, "usb_hid_lib", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(usbClientTask, "usb_hid_cli", 4096, nullptr, 5, nullptr, 0);
  gBeganOk = true;
  return true;
}

bool UsbHidKeyboardHost::connected() const { return gConnected; }

bool UsbHidKeyboardHost::popKey(uint8_t& keyCode, uint8_t& modifiers, bool& pressed) {
  if (gKeyQueue == nullptr) return false;
  KeyMsg msg;
  if (xQueueReceive(gKeyQueue, &msg, 0) != pdTRUE) return false;
  keyCode = msg.keyCode;
  modifiers = msg.modifiers;
  pressed = true;  // only press events are ever queued -- see file header
  return true;
}

bool UsbHidKeyboardHost::beganOk() const { return gBeganOk; }
uint32_t UsbHidKeyboardHost::devicesSeen() const { return gDevicesSeen; }
uint32_t UsbHidKeyboardHost::rejectedCount() const { return gRejectedCount; }
uint32_t UsbHidKeyboardHost::reportsReceived() const { return gReportsReceived; }
uint8_t UsbHidKeyboardHost::lastKeyCode() const { return gLastKeyCode; }
uint8_t UsbHidKeyboardHost::lastModifiers() const { return gLastModifiers; }

}  // namespace freeink

#endif  // FREEINK_CAP_USB_HID_KBD_HOST
