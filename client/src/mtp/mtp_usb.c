#include "mtp_usb.h"

#include <malloc.h>
#include <string.h>
#include <stdio.h>

// High/Super speed max packet sizes for the bulk endpoints, per the USB
// 2.0/3.0 specs (512/1024 bytes). No Full-Speed configuration - usb:ds on
// the Switch never actually negotiates Full-Speed as a device, and
// Atmosphère's own MTP implementation (troposphere/haze, see below) doesn't
// configure one either.
#define MTP_PACKET_SIZE_HS 512
#define MTP_PACKET_SIZE_SS 1024
// PTP-over-USB's interrupt endpoint is for async event notifications
// (ObjectAdded, etc) - this responder never actually sends anything over
// it (neither does haze's - grep its source, the endpoint is registered
// and enabled but never written to), but the endpoint's mere *presence* in
// the descriptor set turned out to be load-bearing: real-hardware testing
// with only the two bulk endpoints got the device to enumerate and Windows'
// WPD/MTP class driver to bind cleanly, but the driver host then crashed
// (Event Viewer 10110/10116, "User-mode Driver problems") before ever
// issuing a single control request or PTP command - consistent with the
// USB Still Image class driver validating the interface shape (2 bulk + 1
// interrupt, matching the PTP-over-USB Bulk-Only Transport spec) before
// attempting to actually talk to the device.
#define MTP_INTERRUPT_PACKET_SIZE 0x18

// Bulk transfer buffers, and the alignment that makes them work at all.
// usbDsEndpoint_PostBufferAsync hands the buffer straight to USB DMA, which
// requires a page-aligned (0x1000) address - haze declares its own as
// `alignas(4_KB) u8 usb_bulk_{read,write}_buffer[...]` for exactly this
// reason. This module originally posted caller-supplied pointers directly,
// which for a stack struct (mtp_ptp.c read PTP's 12-byte container header
// into a local) is essentially never page-aligned: every transfer silently
// failed to complete, which is why real-hardware testing saw USB enumerate
// perfectly, Windows bind its WPD/MTP driver and report the device as
// working, and yet not one PTP command ever reach the responder (the
// on-screen debug counter stayed at 0 through every descriptor-level fix
// attempted before this one). Everything now bounces through these, so
// callers can pass whatever pointer is convenient.
#define MTP_USB_BUFFER_SIZE (1024 * 1024) // matches haze's UsbBulkPacketBufferSize
#define MTP_USB_BUFFER_ALIGN 0x1000

static UsbDsInterface *s_interface = NULL;
static UsbDsEndpoint *s_endpoint_in = NULL;
static UsbDsEndpoint *s_endpoint_out = NULL;
static UsbDsEndpoint *s_endpoint_interrupt = NULL;
static u8 *s_read_buffer = NULL;
static u8 *s_write_buffer = NULL;
static bool s_initialized = false;

static Result append_descriptor(UsbDeviceSpeed speed, const void *descriptor, size_t size) {
    return usbDsInterface_AppendConfigurationData(s_interface, speed, (void *)descriptor, size);
}

bool mtp_usb_init(char *err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    if (!s_read_buffer) s_read_buffer = memalign(MTP_USB_BUFFER_ALIGN, MTP_USB_BUFFER_SIZE);
    if (!s_write_buffer) s_write_buffer = memalign(MTP_USB_BUFFER_ALIGN, MTP_USB_BUFFER_SIZE);
    if (!s_read_buffer || !s_write_buffer) {
        if (err_buf) snprintf(err_buf, err_buf_size, "sin memoria para los búferes USB (2 x %d KB)",
                              MTP_USB_BUFFER_SIZE / 1024);
        mtp_usb_exit();
        return false;
    }

    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "usbDsInitialize falló (0x%x)", rc);
        return false;
    }

    u8 idx_lang = 0;
    u16 lang_en_us = 0x0409;
    usbDsAddUsbLanguageStringDescriptor(&idx_lang, &lang_en_us, 1);

    u8 idx_manufacturer = 0, idx_product = 0, idx_serial = 0;
    usbDsAddUsbStringDescriptor(&idx_manufacturer, "Nintendo");
    usbDsAddUsbStringDescriptor(&idx_product, "Nintendo Switch");
    usbDsAddUsbStringDescriptor(&idx_serial, "FreeShopMTP");

    // idVendor/idProduct: same pair Atmosphère's own haze uses (0x057e is
    // Nintendo's VID; 0x201d is, per haze's own source comment, "a VID:PID
    // recognized by libmtp"). Not just cosmetic: every attempt on this
    // device's arbitrary 0x3999 PID hit the same "no se puede obtener
    // acceso... 0x80070651" failure regardless of how closely the rest of
    // the descriptor set matched haze's - while haze itself, on the same
    // PC/cable/port, has always worked. Windows caches driver/devnode state
    // per VID:PID, so a PID that only this responder's earlier (genuinely
    // broken) attempts ever used may have a bad cached devnode behind it
    // that no amount of fixing the descriptors sent on a *new* connection
    // will clear - reusing haze's already-proven-good PID sidesteps that
    // entirely. If this alone doesn't fix it, the remaining cached-devnode
    // fix is on the Windows side: Device Manager -> Ver -> Mostrar
    // dispositivos ocultos -> buscar el dispositivo bajo Dispositivos
    // portátiles/Otros dispositivos -> Desinstalar dispositivo, marcando
    // "Eliminar el software de controlador para este dispositivo".
    struct usb_device_descriptor device_descriptor = {
        .bLength = USB_DT_DEVICE_SIZE,
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = 0x00,     // class comes from the interface, not the device, for this descriptor style
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 0x40,
        .idVendor = 0x057E,
        .idProduct = 0x201D,
        .bcdDevice = 0x0100,
        .iManufacturer = idx_manufacturer,
        .iProduct = idx_product,
        .iSerialNumber = idx_serial,
        .bNumConfigurations = 1,
    };

    // Each step is named here so a failure says exactly which usbDs* call
    // it was, not just an opaque result code - the whole point being that
    // this can only really be debugged from what real hardware reports
    // back (see mtp_usb.h's design note). This whole sequence (device
    // descriptor per speed, BOS, interface+endpoint layout, endpoint
    // address formula) mirrors Atmosphère's own MTP homebrew
    // (troposphere/haze, source/usb_session.cpp - a real, Windows-proven
    // MTP implementation, unlike vendor-protocol tools like DBI/nxdumptool
    // whose USB code was never trying to satisfy Windows' MTP class driver
    // in the first place) rather than being reconstructed from the USB
    // spec alone.
    const char *step = "usbDsSetUsbDeviceDescriptor(High)";
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);
    if (R_FAILED(rc)) goto fail;
    // SuperSpeed devices report bMaxPacketSize0 as an exponent (2^9 = 512),
    // not a literal byte count - a USB 3.0 spec requirement the High/Full
    // encoding doesn't have.
    device_descriptor.bcdUSB = 0x0300;
    device_descriptor.bMaxPacketSize0 = 0x09;
    step = "usbDsSetUsbDeviceDescriptor(Super)";
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);
    if (R_FAILED(rc)) goto fail;

    // A SuperSpeed device descriptor (bcdUSB=0x0300 above) obligates a BOS
    // descriptor advertising USB 2.0 Extension + SuperSpeed Device
    // Capability - without one, Windows' USB stack considers the SuperSpeed
    // configuration non-compliant. Byte layout copied verbatim from haze's
    // usb_session.cpp, which itself is the standard fixed BOS blob every
    // usb:ds-based Switch homebrew that advertises Super speed uses.
    static const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0a, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0c, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    step = "usbDsSetBinaryObjectStore";
    rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
    if (R_FAILED(rc)) goto fail;

    step = "usbDsRegisterInterface";
    rc = usbDsRegisterInterface(&s_interface);
    if (R_FAILED(rc)) goto fail;

    u8 idx_interface = 0;
    step = "usbDsAddUsbStringDescriptor(MTP)";
    rc = usbDsAddUsbStringDescriptor(&idx_interface, "MTP");
    if (R_FAILED(rc)) goto fail;

    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = s_interface->interface_index,
        .bAlternateSetting = 0,
        .bNumEndpoints = 3, // bulk IN + bulk OUT + interrupt IN - see MTP_INTERRUPT_PACKET_SIZE's doc comment
        .bInterfaceClass = USB_CLASS_IMAGE, // 6 - "Still Image", the class every PTP/MTP device uses
        .bInterfaceSubClass = 0x01,         // Still Image
        .bInterfaceProtocol = 0x01,         // Picture Transfer Protocol (Bulk-Only)
        .iInterface = idx_interface,
    };
    struct usb_endpoint_descriptor endpoint_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = MTP_PACKET_SIZE_HS,
        .bInterval = 0,
    };
    struct usb_endpoint_descriptor endpoint_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = MTP_PACKET_SIZE_HS,
        .bInterval = 0,
    };
    struct usb_endpoint_descriptor endpoint_interrupt = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = MTP_INTERRUPT_PACKET_SIZE,
        // 0x6 the whole time (not just for Super) - haze's Initialize5x
        // (the modern-firmware path any real hardware today takes; only
        // its legacy pre-5.0.0 Initialize1x uses 0x4, a function this port
        // never had a reason to follow) never changes this value between
        // its High and Super AppendConfigurationData calls.
        .bInterval = 0x6,
    };
    // SuperSpeed endpoints additionally need a Companion descriptor right
    // after each endpoint descriptor (USB 3.0 spec). bMaxBurst=0x0f on the
    // bulk companion matches haze exactly; the interrupt companion is the
    // spec-minimum (no burst, no extra bytes/interval).
    struct usb_ss_endpoint_companion_descriptor endpoint_companion = {
        .bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE,
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0x0f,
        .bmAttributes = 0,
        .wBytesPerInterval = 0,
    };
    struct usb_ss_endpoint_companion_descriptor endpoint_companion_interrupt = {
        .bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE,
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0,
        .bmAttributes = 0,
        .wBytesPerInterval = 0,
    };

    // Endpoint addresses aren't just the bare direction bit - the bulk pair
    // shares one endpoint number (interface_number + 1), the interrupt
    // endpoint gets the next one (interface_number + 2), matching haze's
    // own formula.
    endpoint_in.bEndpointAddress = (u8)(endpoint_in.bEndpointAddress + interface_descriptor.bInterfaceNumber + 1);
    endpoint_out.bEndpointAddress = (u8)(endpoint_out.bEndpointAddress + interface_descriptor.bInterfaceNumber + 1);
    endpoint_interrupt.bEndpointAddress = (u8)(endpoint_interrupt.bEndpointAddress + interface_descriptor.bInterfaceNumber + 2);

    // Each speed's configuration is built from separate AppendConfigurationData
    // calls, one descriptor at a time (interface, then each endpoint - plus
    // a companion descriptor after each endpoint at Super speed) - not one
    // call with every descriptor pre-concatenated into a single buffer.
    step = "usbDsInterface_AppendConfigurationData(High, interface)";
    rc = append_descriptor(UsbDeviceSpeed_High, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(High, IN)";
    rc = append_descriptor(UsbDeviceSpeed_High, &endpoint_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(High, OUT)";
    rc = append_descriptor(UsbDeviceSpeed_High, &endpoint_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(High, interrupt)";
    rc = append_descriptor(UsbDeviceSpeed_High, &endpoint_interrupt, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto fail;

    endpoint_in.wMaxPacketSize = endpoint_out.wMaxPacketSize = MTP_PACKET_SIZE_SS;
    step = "usbDsInterface_AppendConfigurationData(Super, interface)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(Super, IN)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &endpoint_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(Super, IN companion)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(Super, OUT)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &endpoint_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(Super, OUT companion)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(Super, interrupt)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &endpoint_interrupt, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_AppendConfigurationData(Super, interrupt companion)";
    rc = append_descriptor(UsbDeviceSpeed_Super, &endpoint_companion_interrupt, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) goto fail;

    step = "usbDsInterface_RegisterEndpoint(IN)";
    rc = usbDsInterface_RegisterEndpoint(s_interface, &s_endpoint_in, endpoint_in.bEndpointAddress);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_RegisterEndpoint(OUT)";
    rc = usbDsInterface_RegisterEndpoint(s_interface, &s_endpoint_out, endpoint_out.bEndpointAddress);
    if (R_FAILED(rc)) goto fail;
    step = "usbDsInterface_RegisterEndpoint(interrupt)";
    rc = usbDsInterface_RegisterEndpoint(s_interface, &s_endpoint_interrupt, endpoint_interrupt.bEndpointAddress);
    if (R_FAILED(rc)) goto fail;

    step = "usbDsInterface_EnableInterface";
    rc = usbDsInterface_EnableInterface(s_interface);
    if (R_FAILED(rc)) goto fail;

    step = "usbDsEnable";
    rc = usbDsEnable();
    if (R_FAILED(rc)) goto fail;

    s_initialized = true;
    return true;

fail:
    if (err_buf) {
        snprintf(err_buf, err_buf_size, "%s falló (0x%x, módulo %d, descripción %d)", step, rc,
                 R_MODULE(rc), R_DESCRIPTION(rc));
    }
    mtp_usb_exit();
    return false;
}

void mtp_usb_exit(void) {
    // usbDsExit() itself closes any interfaces/endpoints still open (see
    // its own doc comment) - explicit *_Close calls first anyway, so this
    // is safe to call after a partial/failed init too.
    if (s_endpoint_in) { usbDsEndpoint_Close(s_endpoint_in); s_endpoint_in = NULL; }
    if (s_endpoint_out) { usbDsEndpoint_Close(s_endpoint_out); s_endpoint_out = NULL; }
    if (s_endpoint_interrupt) { usbDsEndpoint_Close(s_endpoint_interrupt); s_endpoint_interrupt = NULL; }
    if (s_interface) { usbDsInterface_Close(s_interface); s_interface = NULL; }
    usbDsExit();
    if (s_read_buffer) { free(s_read_buffer); s_read_buffer = NULL; }
    if (s_write_buffer) { free(s_write_buffer); s_write_buffer = NULL; }
    s_initialized = false;
}

bool mtp_usb_is_connected(void) {
    if (!s_initialized) return false;
    UsbState state = UsbState_Detached;
    if (R_FAILED(usbDsGetState(&state))) return false;
    return state == UsbState_Configured;
}

static int do_transfer(UsbDsEndpoint *ep, void *buf, size_t size, u64 timeout_ns) {
    if (!s_initialized || !ep || size == 0) return -1;

    u32 urb_id = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(ep, buf, size, &urb_id);
    if (R_FAILED(rc)) return -1;

    rc = eventWait(&ep->CompletionEvent, timeout_ns);
    if (R_FAILED(rc)) {
        // Timeout or the cable was pulled mid-wait - cancel the pending
        // transfer so it doesn't complete into a buffer the caller's
        // already moved on from.
        usbDsEndpoint_Cancel(ep);
        eventWait(&ep->CompletionEvent, 1000000000ULL); // let the cancel's own completion drain
        eventClear(&ep->CompletionEvent);
        return rc == KERNELRESULT(TimedOut) ? 0 : -1;
    }
    eventClear(&ep->CompletionEvent);

    UsbDsReportData report;
    rc = usbDsEndpoint_GetReportData(ep, &report);
    if (R_FAILED(rc)) return -1;

    u32 requested = 0, transferred = 0;
    rc = usbDsParseReportData(&report, urb_id, &requested, &transferred);
    if (R_FAILED(rc)) return -1;

    return (int)transferred;
}

int mtp_usb_read(void *buf, size_t size, u64 timeout_ns) {
    if (!s_read_buffer) return -1;
    if (size > MTP_USB_BUFFER_SIZE) size = MTP_USB_BUFFER_SIZE;

    int transferred = do_transfer(s_endpoint_out, s_read_buffer, size, timeout_ns);
    if (transferred > 0) memcpy(buf, s_read_buffer, (size_t)transferred);
    return transferred;
}

int mtp_usb_write(const void *buf, size_t size, u64 timeout_ns) {
    if (!s_write_buffer) return -1;
    if (size > MTP_USB_BUFFER_SIZE) return -1; // caller's job to chunk - silently truncating a write corrupts the stream

    memcpy(s_write_buffer, buf, size);
    return do_transfer(s_endpoint_in, s_write_buffer, size, timeout_ns);
}
