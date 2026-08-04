#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

// USB transport for the MTP responder (mtp_ptp.h) - one bulk OUT endpoint
// (host->console commands/data), one bulk IN endpoint (console->host
// responses/data), and one interrupt IN endpoint (registered/enabled but
// never written to by this responder - see mtp_usb.c's doc comment on
// MTP_INTERRUPT_PACKET_SIZE for why it still has to exist), presented
// under the standard USB Still Image (PTP) class (bInterfaceClass=6/
// subclass=1/protocol=1) - the class every real PTP/MTP camera and
// portable device uses, which Windows/macOS/Linux all have a built-in
// driver for (no INF, no companion PC app).
//
// What this does NOT (yet) do: the Microsoft OS descriptor dance (either
// the legacy string-index-0xEE trick or the newer BOS-based MS OS 2.0
// descriptor) some MTP devices use to steer Windows towards its WPD
// (Portable Devices/file-browsing) driver instead of its WIA (camera
// photo-import) one. Whether that's actually needed here is unconfirmed -
// the MTP vendor-extension signature in GetDeviceInfo (see mtp_ptp.c) is
// the well-documented, protocol-level way devices identify as MTP rather
// than plain PTP, and *should* be enough on its own; if real-hardware
// testing shows Windows treating this as a camera instead of a drive
// (check Device Manager: "Portable Devices" vs "Imaging devices" vs
// "Other devices"), that's the concrete signal to add the OS descriptor on
// top of this rather than guessing at it now.

// Registers the USB interface (descriptors + two bulk endpoints) with
// usb:ds and enables it. Returns false with a reason in err_buf on any
// usbDs* call failing - none of these should normally fail on real
// hardware, so a failure here likely means another sysmodule/homebrew
// already has the USB device claimed.
bool mtp_usb_init(char *err_buf, size_t err_buf_size);
void mtp_usb_exit(void);

// True once the host has connected and configured the USB device (cable
// plugged into a live USB port, drivers bound) - doesn't mean a PTP
// session is open yet, just that the transport is ready for one.
bool mtp_usb_is_connected(void);

// Blocking bulk transfers (each call is exactly one PTP container, or one
// chunk of a large data phase - see mtp_ptp.c). `timeout_ns` bounds the
// wait so a caller polling this from a UI loop (checking for cancel/B
// every frame) doesn't stall indefinitely with nothing connected. Returns
// the byte count actually transferred, 0 on timeout, or -1 on a real
// transport error (cable unplugged mid-transfer, endpoint stalled, etc.) -
// callers should treat -1 as "the session is over".
int mtp_usb_read(void *buf, size_t size, u64 timeout_ns);
int mtp_usb_write(const void *buf, size_t size, u64 timeout_ns);
