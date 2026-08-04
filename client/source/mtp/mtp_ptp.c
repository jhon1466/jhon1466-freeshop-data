#include "mtp_ptp.h"
#include "mtp_usb.h"
#include "../config.h" // SWITCH_APPS_ROOT
#include "../install/install_common.h"
#include "../install/install_local.h"
#include "../install/install_stream.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>

#define MTP_LANDING_DIR "sdmc:/switch/freeshop/mtp_incoming"
#define MTP_STORAGE_ID 0x00010001u

// Short poll while idle (no command in flight) - keeps mtp_step() cheap to
// call once per rendered frame. Long timeout is for reads that are known
// to have more coming (mid data-phase) or where the host is expected to
// respond promptly (right after we've sent something) - a real MTP client
// doesn't sit idle mid-transaction, so a long wait there just means
// something's actually wrong (cable pulled, host hung).
#define MTP_POLL_TIMEOUT_NS 200000000ULL
#define MTP_LONG_TIMEOUT_NS 10000000000ULL

// Same on-wire layout as PTP's Generic Container header (PTP 1.0 / USB
// Still Image spec) - every command/data/response starts with exactly
// this, 12 bytes.
typedef struct {
    uint32_t length;
    uint16_t type; // 1=Command, 2=Data, 3=Response, 4=Event
    uint16_t code;
    uint32_t transaction_id;
} __attribute__((packed)) PtpContainer;
_Static_assert(sizeof(PtpContainer) == 12, "PtpContainer must be 12 bytes");

#define PTP_TYPE_COMMAND 1
#define PTP_TYPE_DATA 2
#define PTP_TYPE_RESPONSE 3

#define PTP_OP_GET_DEVICE_INFO 0x1001
#define PTP_OP_OPEN_SESSION 0x1002
#define PTP_OP_CLOSE_SESSION 0x1003
#define PTP_OP_GET_STORAGE_IDS 0x1004
#define PTP_OP_GET_STORAGE_INFO 0x1005
#define PTP_OP_GET_OBJECT_HANDLES 0x1007
#define PTP_OP_GET_OBJECT_INFO 0x1008
#define PTP_OP_DELETE_OBJECT 0x100B
#define PTP_OP_SEND_OBJECT_INFO 0x100C
#define PTP_OP_SEND_OBJECT 0x100D
// MTP object-properties operations - Windows uses GetObjectPropDesc on
// ObjectSize specifically to check whether this device reports sizes as a
// 64-bit value (see PTP_PROP_OBJECT_SIZE's handling in
// PTP_OP_GET_OBJECT_PROP_DESC below) before it will attempt pushing a file
// over 4GB at all - without this, and the Android operations below,
// Windows silently refuses/truncates any transfer that big.
#define PTP_OP_GET_OBJECT_PROPS_SUPPORTED 0x9801
#define PTP_OP_GET_OBJECT_PROP_DESC 0x9802
#define PTP_OP_GET_OBJECT_PROP_VALUE 0x9803
#define PTP_OP_SET_OBJECT_PROP_VALUE 0x9804
#define PTP_OP_GET_OBJECT_PROP_LIST 0x9805
// SendObjectInfo's replacement for anything over 4GB, and the only way this
// responder ever learns such a file's real size: its ObjectSize travels as a
// 64-bit value in the *command parameters* (see the handler for the exact
// slot order), not in a 32-bit dataset field. A host only uses it if the
// device lists it here - without it Windows falls back to SendObjectInfo
// with a 0xFFFFFFFF "can't tell you" size, which is why a >4GB transfer used
// to run with no total to show progress against.
#define PTP_OP_SEND_OBJECT_PROP_LIST 0x9808
// Android's MTP large-object extension - the actual >4GB transfer mechanism:
// SendObjectInfo's own ObjectCompressedSize field is 32-bit, so a host that
// knows a file exceeds that sends 0xFFFFFFFF there instead and pushes the
// real data through this sequence rather than plain SendObject.
#define PTP_OP_ANDROID_GET_PARTIAL_OBJECT_64 0x95C1
#define PTP_OP_ANDROID_SEND_PARTIAL_OBJECT 0x95C2
#define PTP_OP_ANDROID_TRUNCATE_OBJECT 0x95C3
#define PTP_OP_ANDROID_BEGIN_EDIT_OBJECT 0x95C4
#define PTP_OP_ANDROID_END_EDIT_OBJECT 0x95C5

#define PTP_RC_OK 0x2001
#define PTP_RC_GENERAL_ERROR 0x2002
#define PTP_RC_OPERATION_NOT_SUPPORTED 0x2005
#define PTP_RC_INVALID_OBJECT_HANDLE 0x2009
#define PTP_RC_INVALID_PARAMETER 0x201D
#define PTP_RC_INVALID_OBJECT_PROP_CODE 0xA801 // MTP extension - "unknown/unsupported object property"

// MTP object property codes (PTP_OP_GET_OBJECT_PROP_* / PTP_OP_SET_OBJECT_PROP_VALUE) -
// the subset haze itself declares, which is what real-hardware testing
// against Windows is calibrated against.
#define PTP_PROP_STORAGE_ID 0xDC01
#define PTP_PROP_OBJECT_FORMAT 0xDC02
#define PTP_PROP_OBJECT_SIZE 0xDC04
#define PTP_PROP_OBJECT_FILENAME 0xDC07
#define PTP_PROP_PARENT_OBJECT 0xDC0B
#define PTP_PROP_PERSISTENT_UID 0xDC41

// PTP property dataset type codes (PTP 1.0 spec Annex).
#define PTP_DTC_U16 0x0004
#define PTP_DTC_U32 0x0006
#define PTP_DTC_U64 0x0008
#define PTP_DTC_U128 0x000A
#define PTP_DTC_STRING 0xFFFF
#define PTP_PROP_GETSET_GET 0x00
#define PTP_PROP_GETSET_GETSET 0x01

static bool s_running = false;
static bool s_session_open = false;
static u32 s_next_object_handle = 2; // 1 is never used - keeps 0/1 unambiguous as "no handle yet"

// Where the SendObject/SendPartialObject data phase actually goes depends
// on the file. An .nsp/.nsz installs straight into a streaming installer
// (install_stream.h) as it arrives; an .xci/.xcz the same, just through the
// nested-partition state machine install_stream.h also handles; anything
// else is staged to a file the way this always did. Either way the payload
// is never buffered whole in memory.
typedef struct {
    InstallStream *stream; // direct-install sink (.nsp/.nsz/.xci/.xcz); NULL when staging
    FILE *fp;               // staging sink; NULL when streaming
    char err[200];
} ObjectSink;

// One object created by SendObjectInfo, tracked from there through however
// its data actually arrives - either a single synchronous SendObject (the
// common, <4GB case), or the Android large-object sequence
// (BeginEditObject, one or more SendPartialObject calls, TruncateObject,
// EndEditObject) a host uses instead when it already knows the file is too
// big for SendObjectInfo's own 32-bit size field. Every operation that
// takes an object_id (GetObjectInfo, the *ObjectProp* operations,
// DeleteObject, and the whole Android sequence) only ever recognizes
// `handle` here - this responder is a drop target, not a real file
// browser, so it never has more than one real object at a time.
typedef struct {
    bool active;      // SendObjectInfo ran, this slot describes a real object (in flight or finished)
    bool sink_open;    // the receive sink itself has been opened (SendObject, or BeginEditObject)
    // Set once the object has been fully received and installed. The record
    // deliberately stays `active` afterwards: a host that has just finished
    // writing a file immediately asks about it again (GetObjectInfo, the
    // *ObjectProp* operations) to refresh its own view, and answering
    // "invalid handle" to those is read as the copy having failed.
    bool completed;
    char filename[256];
    u32 handle;
    // The object's real size, and whether it's actually known. SendObjectInfo
    // can only carry 32 bits and sends 0xFFFFFFFF when the truth doesn't fit
    // (leaving this unknown); SendObjectPropList carries a real 64-bit value.
    uint64_t declared_size;
    bool size_known;
    uint64_t truncated_size; // set by TruncateObject once known, 0 until then
    uint64_t received;       // bytes fed into sink so far - the large path's offset-continuity anchor
    char dest_path[400];     // only meaningful for a staged (unsupported-format) sink
    ObjectSink sink;
} PendingReceive;
static PendingReceive s_recv;

// ---- Byte-packing helpers for PTP datasets - every field is little-endian,
// matching ARM64, so these are just typed memcpys with a cursor. ----
static void put_u16(uint8_t **p, uint16_t v) { memcpy(*p, &v, 2); *p += 2; }
static void put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
static void put_u64(uint8_t **p, uint64_t v) { memcpy(*p, &v, 8); *p += 8; }
static void put_u8(uint8_t **p, uint8_t v) { **p = v; *p += 1; }

// PTP String: a 1-byte character count *including* the terminating NUL,
// then that many UTF-16LE code units (count=0 means empty, no data
// follows). Every string this responder sends is a plain ASCII literal, so
// this widens each byte to UTF-16 rather than needing a real UTF-8 decoder.
static void put_string(uint8_t **p, const char *ascii) {
    size_t len = strlen(ascii);
    if (len > 254) len = 254; // count byte is 1 byte (max 255) - leave room for the NUL
    if (len == 0) { **p = 0; *p += 1; return; }
    **p = (uint8_t)(len + 1);
    *p += 1;
    for (size_t i = 0; i < len; i++) put_u16(p, (uint16_t)(unsigned char)ascii[i]);
    put_u16(p, 0);
}

static void put_u16_array(uint8_t **p, const uint16_t *items, uint32_t count) {
    put_u32(p, count);
    for (uint32_t i = 0; i < count; i++) put_u16(p, items[i]);
}

// Reads command parameter slot `index` (0-based, 4 bytes each) as a u32.
// Returns 0 if the packet was too short to contain it - every caller here
// treats a too-short param list as "parameter is 0/absent" rather than a
// transport error, matching how little these parameters are actually
// trusted (object_id is always cross-checked against s_recv.handle; a
// wrong-but-present-looking 0 just fails that check like any other mismatch).
static u32 read_param_u32(const uint8_t *params, size_t params_len, int index) {
    size_t off = (size_t)index * 4;
    if (off + 4 > params_len) return 0;
    u32 v;
    memcpy(&v, params + off, 4);
    return v;
}

// Reads a u64 spanning two consecutive 4-byte parameter slots starting at
// `index` - how the Android large-object operations (SendPartialObject's
// offset, TruncateObject's size) pack a 64-bit value into PTP's otherwise
// 32-bit-per-parameter command format. Verified against Atmosphère's own
// haze (troposphere/haze/source/ptp_responder_android_operations.cpp) byte
// for byte, including which slot each operation's u64 actually starts at -
// getting this wrong wouldn't just misbehave, it'd silently write file data
// at the wrong offset.
static u64 read_param_u64(const uint8_t *params, size_t params_len, int index) {
    size_t off = (size_t)index * 4;
    if (off + 8 > params_len) return 0;
    u64 v;
    memcpy(&v, params + off, 8);
    return v;
}

// Reads a u64 split across two parameter slots *high word first* - which is
// how SendObjectPropList packs ObjectSize, and the opposite of the
// contiguous-little-endian layout read_param_u64 above handles. Checked
// against libmtp's own ptp_mtp_sendobjectproplist, which builds the command
// as `Param4 = objectsize >> 32; Param5 = objectsize & 0xffffffff`. Reading
// these two slots as one little-endian u64 instead would swap the halves and
// yield an absurd size - for a 5GB file, ~1 byte.
static u64 read_param_u64_hi_lo(const uint8_t *params, size_t params_len, int hi_index) {
    size_t off = (size_t)hi_index * 4;
    if (off + 8 > params_len) return 0;
    u32 hi, lo;
    memcpy(&hi, params + off, 4);
    memcpy(&lo, params + off + 4, 4);
    return ((u64)hi << 32) | lo;
}

// ---- Container I/O ----

static bool send_response(u16 code, u32 transaction_id, const u32 *params, int nparams) {
    uint8_t buf[12 + 5 * 4];
    PtpContainer hdr = { .length = (uint32_t)(12 + nparams * 4), .type = PTP_TYPE_RESPONSE,
                          .code = code, .transaction_id = transaction_id };
    memcpy(buf, &hdr, 12);
    for (int i = 0; i < nparams; i++) memcpy(buf + 12 + i * 4, &params[i], 4);
    return mtp_usb_write(buf, 12 + (size_t)nparams * 4, MTP_LONG_TIMEOUT_NS) == 12 + nparams * 4;
}

// Header and body go out as one transfer, not two. A PTP container is one
// logical unit on the wire, and splitting it would put a short packet
// (12 bytes, under the 512-byte endpoint max) between the two halves -
// which is exactly how USB signals "this transfer is over", so a host
// would see a truncated container followed by a stray one.
static bool send_data(u16 code, u32 transaction_id, const uint8_t *data, size_t len) {
    static uint8_t packet[1024];
    if (12 + len > sizeof(packet)) return false;

    PtpContainer hdr = { .length = (uint32_t)(12 + len), .type = PTP_TYPE_DATA,
                          .code = code, .transaction_id = transaction_id };
    memcpy(packet, &hdr, 12);
    if (len > 0) memcpy(packet + 12, data, len);
    return mtp_usb_write(packet, 12 + len, MTP_LONG_TIMEOUT_NS) == (int)(12 + len);
}

// Reads one small Data-phase container in one shot - used for
// SendObjectInfo's ObjectInfo dataset (well under a KB, never a large file
// payload, which streams separately in receive_object). The container
// header arrives in the same transfer as the body it describes, so both
// come out of a single read.
static bool read_small_data_phase(uint8_t *buf, size_t buf_cap, size_t *out_len) {
    static uint8_t packet[4096];
    int n = mtp_usb_read(packet, sizeof(packet), MTP_LONG_TIMEOUT_NS);
    if (n < (int)sizeof(PtpContainer)) return false;

    PtpContainer hdr;
    memcpy(&hdr, packet, sizeof(hdr));
    if (hdr.type != PTP_TYPE_DATA) return false;

    size_t body_len = (size_t)n - sizeof(hdr);
    if (body_len > buf_cap) body_len = buf_cap;
    if (body_len > 0) memcpy(buf, packet + sizeof(hdr), body_len);
    *out_len = body_len;
    return true;
}

// A command with a data phase this responder doesn't need the contents of
// (SetObjectPropValue - we reject renames, but still have to consume the
// bytes the host already committed to sending) - reads and discards it so
// the next command's read lines up on the next container instead of the
// tail of this one's data.
static void drain_data_phase(void) {
    static uint8_t discard[4096];
    mtp_usb_read(discard, sizeof(discard), MTP_LONG_TIMEOUT_NS);
}

// ---- GetDeviceInfo ----

static const uint16_t kOperationsSupported[] = {
    PTP_OP_GET_DEVICE_INFO, PTP_OP_OPEN_SESSION, PTP_OP_CLOSE_SESSION, PTP_OP_GET_STORAGE_IDS,
    PTP_OP_GET_STORAGE_INFO, PTP_OP_GET_OBJECT_HANDLES, PTP_OP_GET_OBJECT_INFO, PTP_OP_DELETE_OBJECT,
    PTP_OP_SEND_OBJECT_INFO, PTP_OP_SEND_OBJECT,
    PTP_OP_GET_OBJECT_PROPS_SUPPORTED, PTP_OP_GET_OBJECT_PROP_DESC, PTP_OP_GET_OBJECT_PROP_VALUE,
    PTP_OP_SET_OBJECT_PROP_VALUE, PTP_OP_GET_OBJECT_PROP_LIST, PTP_OP_SEND_OBJECT_PROP_LIST,
    PTP_OP_ANDROID_GET_PARTIAL_OBJECT_64, PTP_OP_ANDROID_SEND_PARTIAL_OBJECT, PTP_OP_ANDROID_TRUNCATE_OBJECT,
    PTP_OP_ANDROID_BEGIN_EDIT_OBJECT, PTP_OP_ANDROID_END_EDIT_OBJECT,
};
static const uint16_t kImageFormats[] = { 0x3000, 0x3001 }; // Undefined (any binary file), Association (folder)
static const uint16_t kObjectPropsSupported[] = {
    PTP_PROP_STORAGE_ID, PTP_PROP_OBJECT_FORMAT, PTP_PROP_OBJECT_SIZE,
    PTP_PROP_OBJECT_FILENAME, PTP_PROP_PARENT_OBJECT, PTP_PROP_PERSISTENT_UID,
};

static size_t build_device_info(uint8_t *buf) {
    uint8_t *p = buf;
    put_u16(&p, 100);                          // StandardVersion
    put_u32(&p, 6);                            // VendorExtensionID - Microsoft, the MTP signature within plain PTP
    put_u16(&p, 100);                          // VendorExtensionVersion
    put_string(&p, "microsoft.com: 1.0");      // VendorExtensionDesc - what actually marks this as MTP, not just PTP
    put_u16(&p, 0);                            // FunctionalMode
    put_u16_array(&p, kOperationsSupported, sizeof(kOperationsSupported) / sizeof(uint16_t));
    put_u16_array(&p, NULL, 0);                // EventsSupported - none, this responder never pushes async events
    put_u16_array(&p, NULL, 0);                // DevicePropertiesSupported - none
    put_u16_array(&p, NULL, 0);                // CaptureFormats - none, this isn't a camera
    put_u16_array(&p, kImageFormats, sizeof(kImageFormats) / sizeof(uint16_t));
    put_string(&p, "Nintendo");
    put_string(&p, "Switch");
    put_string(&p, "1.0");
    put_string(&p, "FreeShopMTP");
    return (size_t)(p - buf);
}

// ---- SendObjectInfo's ObjectInfo dataset - only the fields actually used
// (filename, compressed size; everything else the host sends is accepted
// but ignored) - see the PTP 1.0 spec's ObjectInfo layout for the full
// field list this skips over positionally to reach Filename. ----

static bool parse_object_info_filename(const uint8_t *data, size_t len, char *out_name, size_t out_size) {
    // 4(StorageID) + 2+2(ObjectFormat/ProtectionStatus) + 4(CompressedSize) +
    // 2+4+4+4(Thumb) + 4+4+4(Image dims) + 4(ParentObject) + 2+4(Association) +
    // 4(SequenceNumber) = 52 bytes of fixed fields before the first PTP
    // string (Filename).
    const size_t fixed_len = 52;
    if (len < fixed_len + 1) return false;
    size_t off = fixed_len;
    uint8_t char_count = data[off++]; // includes the NUL, 0 = empty name (shouldn't happen for a real file)
    if (char_count == 0) return false;

    size_t n = 0;
    for (int i = 0; i < char_count - 1 && off + 1 < len && n + 1 < out_size; i++) {
        uint16_t code_unit;
        memcpy(&code_unit, data + off, 2);
        off += 2;
        // Every filename this responder needs to round-trip (.nsp/.xci
        // names) is plain ASCII - widen-truncating UTF-16 to a byte here
        // is a deliberate simplification, not a real UTF-16->UTF-8 decode.
        out_name[n++] = (code_unit < 0x80) ? (char)code_unit : '_';
    }
    out_name[n] = '\0';
    return n > 0;
}

// ---- SendObjectPropList's ObjectPropList dataset ----
//
// A flat array of property quadruples rather than ObjectInfo's fixed field
// layout: u32 count, then per element { u32 ObjectHandle (0 for a
// not-yet-created object), u16 PropertyCode, u16 DataType, <value> }. Values
// are variable-width, so walking it means knowing how big each DataType is -
// there's no length prefix per element to skip by.

// Size in bytes of one property value of `datatype` sitting at `data`
// (bounded by `avail`). Returns false if the type isn't one this responder
// understands or the value would run past the buffer - either way the walk
// has to stop, since without a size there's no way to find the next element.
static bool prop_value_size(uint16_t datatype, const uint8_t *data, size_t avail, size_t *out_size) {
    size_t elem = 0;
    switch (datatype & 0x3FFF) { // low bits identify the scalar type; 0x4000 marks an array of it
        case 0x0001: case 0x0002: elem = 1; break;  // INT8 / UINT8
        case 0x0003: case 0x0004: elem = 2; break;  // INT16 / UINT16
        case 0x0005: case 0x0006: elem = 4; break;  // INT32 / UINT32
        case 0x0007: case 0x0008: elem = 8; break;  // INT64 / UINT64
        case 0x0009: case 0x000A: elem = 16; break; // INT128 / UINT128
        default: break;
    }

    if (datatype == PTP_DTC_STRING) {
        if (avail < 1) return false;
        size_t n = 1 + (size_t)data[0] * 2; // count byte + that many UTF-16 code units
        if (n > avail) return false;
        *out_size = n;
        return true;
    }
    if (elem == 0) return false;

    if (datatype & 0x4000) { // array: u32 element count, then that many elements
        if (avail < 4) return false;
        u32 count;
        memcpy(&count, data, 4);
        size_t n = 4 + (size_t)count * elem;
        if (n > avail || (count != 0 && n < 4)) return false; // second test catches the multiply overflowing
        *out_size = n;
        return true;
    }
    if (elem > avail) return false;
    *out_size = elem;
    return true;
}

// Pulls the filename (and, if present, the 64-bit ObjectSize) out of an
// ObjectPropList. `out_size`/`out_size_known` are only written when the list
// actually carries ObjectSize - the command parameters carry it too, so a
// list that omits it isn't an error.
static bool parse_object_prop_list(const uint8_t *data, size_t len, char *out_name, size_t out_name_size,
                                    uint64_t *out_size, bool *out_size_known) {
    if (len < 4) return false;
    u32 count;
    memcpy(&count, data, 4);

    size_t off = 4;
    bool got_name = false;

    for (u32 i = 0; i < count; i++) {
        if (off + 8 > len) break;
        uint16_t property_code, datatype;
        memcpy(&property_code, data + off + 4, 2);
        memcpy(&datatype, data + off + 6, 2);
        off += 8; // past ObjectHandle + PropertyCode + DataType, now at the value

        size_t value_size = 0;
        if (!prop_value_size(datatype, data + off, len - off, &value_size)) break;

        if (property_code == PTP_PROP_OBJECT_FILENAME && datatype == PTP_DTC_STRING) {
            uint8_t char_count = data[off];
            size_t soff = off + 1;
            size_t n = 0;
            for (int c = 0; c < char_count - 1 && soff + 1 < len && n + 1 < out_name_size; c++) {
                uint16_t code_unit;
                memcpy(&code_unit, data + soff, 2);
                soff += 2;
                // Same deliberate ASCII narrowing as parse_object_info_filename.
                out_name[n++] = (code_unit < 0x80) ? (char)code_unit : '_';
            }
            out_name[n] = '\0';
            got_name = n > 0;
        } else if (property_code == PTP_PROP_OBJECT_SIZE && datatype == PTP_DTC_U64 && value_size == 8) {
            uint64_t v;
            memcpy(&v, data + off, 8);
            if (out_size) *out_size = v;
            if (out_size_known) *out_size_known = true;
        }

        off += value_size;
    }

    return got_name;
}

// Builds an ObjectInfo dataset describing `s_recv`'s current object - the
// same field layout parse_object_info_filename reads, just written instead
// of parsed. `size` is capped to 32 bits (the field's own width) regardless
// of the object's real size - a large object's real size only ever travels
// through PTP_PROP_OBJECT_SIZE (64-bit) or TruncateObject, never this.
static size_t build_object_info(uint8_t *buf, u32 size, const char *filename) {
    uint8_t *p = buf;
    put_u32(&p, MTP_STORAGE_ID);
    put_u16(&p, 0x3000); // ObjectFormat - Undefined
    put_u16(&p, 0);      // ProtectionStatus - none
    put_u32(&p, size);
    put_u16(&p, 0);      // ThumbFormat
    put_u32(&p, 0);      // ThumbCompressedSize
    put_u32(&p, 0);      // ThumbWidth
    put_u32(&p, 0);      // ThumbHeight
    put_u32(&p, 0);      // ImageWidth
    put_u32(&p, 0);      // ImageHeight
    put_u32(&p, 0);      // ImageDepth
    put_u32(&p, 0);      // ParentObject - root
    put_u16(&p, 0);      // AssociationType
    put_u32(&p, 0);      // AssociationDesc
    put_u32(&p, 0);      // SequenceNumber
    put_string(&p, filename);
    put_string(&p, "");  // CaptureDate
    put_string(&p, "");  // ModificationDate
    put_string(&p, "");  // Keywords
    return (size_t)(p - buf);
}

// ---- Object receive ----
//
// Where the SendObject/SendPartialObject data phase actually goes depends
// on the file - see ObjectSink's own doc comment.

static bool sink_write(ObjectSink *sink, const uint8_t *data, size_t len) {
    if (len == 0) return true;
    if (sink->stream) {
        return install_stream_feed(sink->stream, data, len, sink->err, sizeof(sink->err));
    }
    return fwrite(data, 1, len, sink->fp) == len;
}

// Reads exactly one Data-phase container (header + up to `expected_size`
// bytes of payload) into `sink`, reporting progress through `progress_cb`
// as `(total, done)`. Used both by plain SendObject (one call, the whole
// object) and by each individual SendPartialObject call (one call per
// chunk of a large object) - the caller decides what `expected_size`,
// `total` and `done`'s starting point mean for its own case.
static bool receive_data_phase(ObjectSink *sink, uint64_t expected_size, uint64_t progress_total,
                                uint64_t progress_base, InstallProgressCallback progress_cb, void *userdata) {
    // The Data container's 12-byte header shares its transfer with the
    // first chunk of payload - so this first read has to keep what follows
    // the header rather than discarding the rest of the packet.
    static uint8_t buf[256 * 1024];
    int first = mtp_usb_read(buf, sizeof(buf), MTP_LONG_TIMEOUT_NS);
    if (first < (int)sizeof(PtpContainer)) return false;

    PtpContainer hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.type != PTP_TYPE_DATA) return false;

    size_t first_payload_len = (size_t)first - sizeof(hdr);

    uint64_t done = 0;
    bool ok = true;

    if (first_payload_len > 0) {
        if ((uint64_t)first_payload_len > expected_size) first_payload_len = (size_t)expected_size;
        if (!sink_write(sink, buf + sizeof(hdr), first_payload_len)) ok = false;
        done += first_payload_len;
        if (ok && progress_cb &&
            !progress_cb((long)progress_total, (long)(progress_base + done), userdata)) ok = false;
    }

    while (ok && done < expected_size) {
        uint64_t remaining = expected_size - done;
        size_t want = remaining < (uint64_t)sizeof(buf) ? (size_t)remaining : sizeof(buf);
        int got = mtp_usb_read(buf, want, MTP_LONG_TIMEOUT_NS);
        if (got <= 0) { ok = false; break; }
        if (!sink_write(sink, buf, (size_t)got)) { ok = false; break; }
        done += (uint64_t)got;
        if (progress_cb &&
            !progress_cb((long)progress_total, (long)(progress_base + done), userdata)) { ok = false; break; }
    }
    return ok;
}

// Same, but for when SendObjectInfo's ObjectCompressedSize was the
// 0xFFFFFFFF sentinel - "the real size doesn't fit in 32 bits, don't ask".
// Real-hardware testing showed Windows using this exact sentinel-then-plain-
// SendObject combination for a >4GB drag-and-drop (never the
// BeginEditObject/SendPartialObject sequence PTP_OP_ANDROID_SEND_PARTIAL_OBJECT
// handles) - and expecting the responder to keep reading until the bulk
// transfer itself ends, not stop at the sentinel's numeric value taken
// literally. Treating 0xFFFFFFFF as a real 4-ish-GB byte count (what this
// used to do) is exactly why a >4GB XCI cut off at "4.00 GB / 4.00 GB" -
// that ceiling was this responder's own doing, not anything Windows or the
// transport actually stopped at.
//
// A USB bulk transfer's end is unambiguous without any pre-known length:
// the sender's last packet is short (fewer bytes than the endpoint's max
// packet size), or - if the true length happens to be an exact multiple of
// that - followed by an explicit zero-length packet. Either one ends this
// loop; nothing here needs to know the real size in advance.
static bool receive_data_phase_unbounded(ObjectSink *sink, InstallProgressCallback progress_cb, void *userdata,
                                          uint64_t *out_received) {
    static uint8_t buf[256 * 1024];
    int first = mtp_usb_read(buf, sizeof(buf), MTP_LONG_TIMEOUT_NS);
    if (first < (int)sizeof(PtpContainer)) return false;

    PtpContainer hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.type != PTP_TYPE_DATA) return false;

    size_t first_payload_len = (size_t)first - sizeof(hdr);
    uint64_t done = 0;
    bool ok = true;
    // Whether the raw read (header + payload together) filled the entire
    // request buffer - if it didn't, the USB layer already gave us a short
    // read, which on a bulk OUT endpoint only happens at the end of the
    // sender's transfer. If it did, there may (or may not) be more still
    // coming; only a follow-up read can tell.
    bool more_may_follow = (first == (int)sizeof(buf));

    if (first_payload_len > 0) {
        if (!sink_write(sink, buf + sizeof(hdr), first_payload_len)) ok = false;
        done += first_payload_len;
        // total=0 here (unknown) - ui_mtp.c's progress callback already
        // falls back to a plain byte count instead of a percentage when
        // told the total isn't known, which is exactly right for this case.
        if (ok && progress_cb && !progress_cb(0, (long)done, userdata)) ok = false;
    }

    while (ok && more_may_follow) {
        int got = mtp_usb_read(buf, sizeof(buf), MTP_LONG_TIMEOUT_NS);
        if (got < 0) { ok = false; break; }
        if (got == 0) break; // explicit zero-length packet - clean end of transfer
        if (!sink_write(sink, buf, (size_t)got)) { ok = false; break; }
        done += (uint64_t)got;
        if (progress_cb && !progress_cb(0, (long)done, userdata)) { ok = false; break; }
        more_may_follow = ((size_t)got == sizeof(buf)); // short packet => that was the last one
    }

    if (ok && out_received) *out_received = done;
    return ok;
}

// ---- Shared sink lifecycle - used by both the plain SendObject path and
// the Android BeginEditObject/SendPartialObject/.../EndEditObject one. ----

// Opens s_recv.sink for s_recv.filename: the streaming installer for
// .nsp/.nsz/.xci/.xcz, a staged file for anything else. `size_hint` only
// matters for the streaming installer's XCI root-partition search window -
// pass 0 when the real size isn't known (see PendingReceive.size_known),
// which just means that search isn't capped to the file's own size.
static bool open_sink_for_recv(uint64_t size_hint) {
    const char *filename = s_recv.filename;
    const char *ext = strrchr(filename, '.');
    bool is_nsp = ext && (strcasecmp(ext, ".nsp") == 0 || strcasecmp(ext, ".nsz") == 0);
    bool is_xci = ext && (strcasecmp(ext, ".xci") == 0 || strcasecmp(ext, ".xcz") == 0);

    memset(&s_recv.sink, 0, sizeof(s_recv.sink));
    s_recv.dest_path[0] = '\0';

    if (is_nsp || is_xci) {
        char err[200];
        s_recv.sink.stream = is_nsp ? install_stream_begin(size_hint, err, sizeof(err))
                                     : install_stream_begin_xci(size_hint, err, sizeof(err));
        if (!s_recv.sink.stream) {
            snprintf(s_recv.sink.err, sizeof(s_recv.sink.err), "%s", err);
            return false;
        }
    } else {
        install_common_mkdir_ignore_exists(SWITCH_APPS_ROOT);
        install_common_mkdir_ignore_exists(SWITCH_APPS_ROOT "/freeshop");
        install_common_mkdir_ignore_exists(MTP_LANDING_DIR);
        snprintf(s_recv.dest_path, sizeof(s_recv.dest_path), "%s/%s", MTP_LANDING_DIR, filename);
        s_recv.sink.fp = fopen(s_recv.dest_path, "wb");
        if (!s_recv.sink.fp) {
            snprintf(s_recv.sink.err, sizeof(s_recv.sink.err), "no se pudo crear %s", s_recv.dest_path);
            return false;
        }
    }
    s_recv.sink_open = true;
    s_recv.received = 0;
    return true;
}

// Records one finished transfer - dropping the oldest entry first if the
// history is already full, so this always reflects the most *recent*
// completions rather than silently refusing new ones past MTP_HISTORY_MAX.
static void push_history(MtpState *state, const char *filename, MtpHistoryStatus status, const char *error) {
    if (state->history_count == MTP_HISTORY_MAX) {
        memmove(&state->history[0], &state->history[1], sizeof(MtpHistoryItem) * (MTP_HISTORY_MAX - 1));
        state->history_count--;
    }
    MtpHistoryItem *item = &state->history[state->history_count++];
    snprintf(item->filename, sizeof(item->filename), "%s", filename);
    item->status = status;
    item->error[0] = '\0';
    if (error) snprintf(item->error, sizeof(item->error), "%s", error);
}

// Closes s_recv.sink and, on a successful receive, runs the actual
// install - recording the outcome in `state`'s history either way. Used
// both when plain SendObject finishes and when EndEditObject closes out a
// large-object upload. Always clears s_recv.active/sink_open. Returns
// whether the object ended up installed, so the caller can answer the
// host accordingly.
static bool finish_recv(bool received_ok, MtpState *state, InstallProgressCallback progress_cb, void *userdata) {
    ObjectSink *sink = &s_recv.sink;
    if (sink->fp) fclose(sink->fp);

    if (!received_ok) {
        if (sink->stream) install_stream_abort(sink->stream);
        else if (s_recv.dest_path[0] != '\0') remove(s_recv.dest_path);
        push_history(state, s_recv.filename, MTP_HISTORY_FAILED,
                     sink->err[0] != '\0' ? sink->err : "recepción interrumpida");
        s_recv.active = false;
        s_recv.sink_open = false;
        state->current_file[0] = '\0';
        return false;
    }

    bool installed = false;
    // Whatever the host actually delivered - reported back from here on, so
    // post-transfer property queries state the real size rather than a
    // declaration that may have been the "doesn't fit in 32 bits" sentinel.
    s_recv.declared_size = s_recv.received;
    s_recv.size_known = true;

    if (sink->stream) {
        // Committing takes a few seconds (reading the CNMT back, writing the
        // meta record, importing tickets) with no USB serviced in between -
        // let the UI say so rather than leaving a finished progress bar on
        // screen looking hung.
        state->status = MTP_STATUS_INSTALLING;
        if (progress_cb) progress_cb(0, 0, userdata);

        char err[200] = "";
        InstallLocalResult res = install_stream_finish(sink->stream, err, sizeof(err));
        if (res == INSTALL_LOCAL_OK) {
            push_history(state, s_recv.filename, MTP_HISTORY_INSTALLED, NULL);
            installed = true;
        } else if (res != INSTALL_LOCAL_ERR_CANCELED) {
            push_history(state, s_recv.filename, MTP_HISTORY_FAILED, err);
        }
    } else {
        push_history(state, s_recv.filename, MTP_HISTORY_FAILED,
                     "recibido pero no instalado (formato no soportado por MTP)");
    }

    // The object exists from the host's point of view either way (it was
    // received in full; only installing it may have failed), so the handle
    // stays answerable - see PendingReceive.completed.
    s_recv.completed = true;
    s_recv.sink_open = false;
    state->current_file[0] = '\0';
    return installed;
}

// ---- Command dispatch ----

static void handle_command(const PtpContainer *cmd, const uint8_t *params, size_t params_len, MtpState *state,
                            InstallProgressCallback progress_cb, void *userdata) {
    switch (cmd->code) {
        case PTP_OP_GET_DEVICE_INFO: {
            static uint8_t buf[512];
            size_t len = build_device_info(buf);
            send_data(cmd->code, cmd->transaction_id, buf, len);
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_OPEN_SESSION:
            s_session_open = true;
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        case PTP_OP_CLOSE_SESSION:
            s_session_open = false;
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        case PTP_OP_GET_STORAGE_IDS: {
            uint8_t buf[8];
            uint8_t *p = buf;
            put_u32(&p, 1);
            put_u32(&p, MTP_STORAGE_ID);
            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_GET_STORAGE_INFO: {
            // Real SD card numbers, same statvfs the installers use for
            // their own space checks - Windows shows these on the device's
            // drive entry, and hardcoded placeholders there are actively
            // misleading (a "64 GB free of 128 GB" that matches no real
            // card, and would let Explorer start a copy that can't fit).
            u64 total_bytes = 0, free_bytes = 0;
            struct statvfs st;
            if (statvfs("sdmc:/", &st) == 0) {
                total_bytes = (u64)st.f_frsize * st.f_blocks;
                free_bytes = (u64)st.f_bsize * st.f_bavail;
            }

            uint8_t buf[96];
            uint8_t *p = buf;
            put_u16(&p, 0x0004);       // StorageType - Fixed RAM (closest fit for "just works")
            put_u16(&p, 0x0002);       // FilesystemType - Generic Hierarchical
            put_u16(&p, 0x0000);       // AccessCapability - ReadWrite
            put_u64(&p, total_bytes);  // MaxCapacity
            put_u64(&p, free_bytes);   // FreeSpaceInBytes
            put_u32(&p, 0xFFFFFFFF);   // FreeSpaceInObjects - not tracked
            put_string(&p, "Tarjeta SD"); // StorageDescription
            put_string(&p, "");           // VolumeLabel
            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_HANDLES: {
            // Always empty - this responder is a drop target, not a file
            // browser (see mtp_ptp.h's design note). A real MTP client
            // still expects a (possibly zero-length) ObjectHandle array
            // back, not an error, when asked to list a valid storage's
            // root.
            uint8_t buf[4];
            uint8_t *p = buf;
            put_u32(&p, 0);
            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_INFO: {
            u32 object_id = read_param_u32(params, params_len, 0);
            // GetObjectHandles above never reports a handle, so the only
            // one a host can meaningfully ask about is the one this
            // responder itself just handed out via SendObjectInfo.
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            // ObjectInfo's own size field is 32-bit, so anything that
            // doesn't fit goes back out as the same 0xFFFFFFFF sentinel a
            // host would have sent us - its real value is only ever
            // readable through PTP_PROP_OBJECT_SIZE (64-bit).
            uint64_t real_size = s_recv.size_known ? s_recv.declared_size : s_recv.received;
            u32 size = real_size > 0xFFFFFFFEu ? 0xFFFFFFFFu : (u32)real_size;
            static uint8_t buf[600];
            size_t len = build_object_info(buf, size, s_recv.filename);
            send_data(cmd->code, cmd->transaction_id, buf, len);
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_DELETE_OBJECT: {
            u32 object_id = read_param_u32(params, params_len, 0);
            // Only ever our own currently-pending object - a host cancelling
            // a copy mid-transfer deletes the partial object it just
            // created, which this responder needs to clean up (an
            // abandoned NCM placeholder or staged file otherwise). Nothing
            // else is ever a valid handle here (see GetObjectInfo above).
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            // An already-installed object can't be undone from here (that
            // would mean uninstalling a title the user now has), so the
            // handle is simply retired - answering OK without touching the
            // install, rather than failing a cleanup the host considers
            // routine.
            if (s_recv.sink_open) {
                if (s_recv.sink.fp) fclose(s_recv.sink.fp);
                if (s_recv.sink.stream) install_stream_abort(s_recv.sink.stream);
                else if (s_recv.dest_path[0] != '\0') remove(s_recv.dest_path);
            }
            s_recv.active = false;
            s_recv.sink_open = false;
            s_recv.completed = false;
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_PROPS_SUPPORTED: {
            uint8_t buf[4 + sizeof(kObjectPropsSupported)];
            uint8_t *p = buf;
            put_u16_array(&p, kObjectPropsSupported, sizeof(kObjectPropsSupported) / sizeof(uint16_t));
            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_PROP_DESC: {
            u32 property_code = read_param_u32(params, params_len, 0);
            uint8_t buf[48];
            uint8_t *p = buf;
            put_u16(&p, (uint16_t)property_code);
            switch (property_code) {
                case PTP_PROP_PERSISTENT_UID: {
                    put_u16(&p, PTP_DTC_U128);
                    put_u8(&p, PTP_PROP_GETSET_GET);
                    uint8_t zero[16] = {0};
                    memcpy(p, zero, sizeof(zero));
                    p += sizeof(zero);
                    break;
                }
                // This is the one Windows actually checks: reporting
                // ObjectSize as a 64-bit datatype here is what tells it a
                // >4GB transfer is possible on this device at all (paired
                // with the Android operations being present in
                // GetDeviceInfo's OperationsSupported).
                case PTP_PROP_OBJECT_SIZE:
                    put_u16(&p, PTP_DTC_U64);
                    put_u8(&p, PTP_PROP_GETSET_GET);
                    put_u64(&p, 0);
                    break;
                case PTP_PROP_STORAGE_ID:
                case PTP_PROP_PARENT_OBJECT:
                    put_u16(&p, PTP_DTC_U32);
                    put_u8(&p, PTP_PROP_GETSET_GET);
                    put_u32(&p, 0);
                    break;
                case PTP_PROP_OBJECT_FORMAT:
                    put_u16(&p, PTP_DTC_U16);
                    put_u8(&p, PTP_PROP_GETSET_GET);
                    put_u16(&p, 0x3000);
                    break;
                case PTP_PROP_OBJECT_FILENAME:
                    put_u16(&p, PTP_DTC_STRING);
                    put_u8(&p, PTP_PROP_GETSET_GETSET);
                    put_string(&p, "");
                    break;
                default:
                    send_response(PTP_RC_INVALID_OBJECT_PROP_CODE, cmd->transaction_id, NULL, 0);
                    return;
            }
            put_u32(&p, 0); // GroupCode - unused, required by the dataset shape
            put_u8(&p, 0);  // FormFlag - None
            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_PROP_VALUE: {
            u32 object_id = read_param_u32(params, params_len, 0);
            u32 property_code = read_param_u32(params, params_len, 1);
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            uint8_t buf[300];
            uint8_t *p = buf;
            switch (property_code) {
                case PTP_PROP_PERSISTENT_UID: {
                    uint8_t val[16] = {0};
                    memcpy(val, &s_recv.handle, sizeof(s_recv.handle));
                    memcpy(p, val, sizeof(val));
                    p += sizeof(val);
                    break;
                }
                case PTP_PROP_OBJECT_SIZE: {
                    uint64_t size = s_recv.truncated_size ? s_recv.truncated_size
                                    : (s_recv.size_known ? s_recv.declared_size : s_recv.received);
                    put_u64(&p, size);
                    break;
                }
                case PTP_PROP_STORAGE_ID: put_u32(&p, MTP_STORAGE_ID); break;
                case PTP_PROP_PARENT_OBJECT: put_u32(&p, 0); break;
                case PTP_PROP_OBJECT_FORMAT: put_u16(&p, 0x3000); break;
                case PTP_PROP_OBJECT_FILENAME: put_string(&p, s_recv.filename); break;
                default:
                    send_response(PTP_RC_INVALID_OBJECT_PROP_CODE, cmd->transaction_id, NULL, 0);
                    return;
            }
            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_SET_OBJECT_PROP_VALUE:
            // Renaming a pending object isn't something installing over
            // MTP needs - reject cleanly, but the data phase the host
            // already committed to sending still has to be drained so the
            // next command's read doesn't consume its tail instead.
            drain_data_phase();
            send_response(PTP_RC_OPERATION_NOT_SUPPORTED, cmd->transaction_id, NULL, 0);
            break;
        case PTP_OP_GET_OBJECT_PROP_LIST: {
            u32 object_id = read_param_u32(params, params_len, 0);
            u32 object_format = read_param_u32(params, params_len, 1);
            int32_t property_code = (int32_t)read_param_u32(params, params_len, 2);
            int32_t group_code = (int32_t)read_param_u32(params, params_len, 3);
            int32_t depth = (int32_t)read_param_u32(params, params_len, 4);

            if (object_format != 0 || group_code != 0 || depth != 0) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd->transaction_id, NULL, 0);
                break;
            }
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            if (property_code != -1 && property_code != PTP_PROP_STORAGE_ID &&
                property_code != PTP_PROP_OBJECT_FORMAT && property_code != PTP_PROP_OBJECT_SIZE &&
                property_code != PTP_PROP_OBJECT_FILENAME && property_code != PTP_PROP_PARENT_OBJECT &&
                property_code != PTP_PROP_PERSISTENT_UID) {
                send_response(PTP_RC_INVALID_OBJECT_PROP_CODE, cmd->transaction_id, NULL, 0);
                break;
            }

            uint64_t size = s_recv.truncated_size ? s_recv.truncated_size
                             : (s_recv.size_known ? s_recv.declared_size : s_recv.received);

            static uint8_t buf[600];
            uint8_t *p = buf;
            uint8_t *count_field = p;
            put_u32(&p, 0); // filled in below, once the real count is known
            u32 n = 0;

            #define EMIT_PROP(code) (property_code == -1 || property_code == (code))
            if (EMIT_PROP(PTP_PROP_STORAGE_ID)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_STORAGE_ID); put_u16(&p, PTP_DTC_U32);
                put_u32(&p, MTP_STORAGE_ID); n++;
            }
            if (EMIT_PROP(PTP_PROP_OBJECT_FORMAT)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_OBJECT_FORMAT); put_u16(&p, PTP_DTC_U16);
                put_u16(&p, 0x3000); n++;
            }
            if (EMIT_PROP(PTP_PROP_OBJECT_SIZE)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_OBJECT_SIZE); put_u16(&p, PTP_DTC_U64);
                put_u64(&p, size); n++;
            }
            if (EMIT_PROP(PTP_PROP_OBJECT_FILENAME)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_OBJECT_FILENAME); put_u16(&p, PTP_DTC_STRING);
                put_string(&p, s_recv.filename); n++;
            }
            if (EMIT_PROP(PTP_PROP_PARENT_OBJECT)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_PARENT_OBJECT); put_u16(&p, PTP_DTC_U32);
                put_u32(&p, 0); n++;
            }
            if (EMIT_PROP(PTP_PROP_PERSISTENT_UID)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_PERSISTENT_UID); put_u16(&p, PTP_DTC_U128);
                uint8_t val[16] = {0};
                memcpy(val, &s_recv.handle, sizeof(s_recv.handle));
                memcpy(p, val, sizeof(val));
                p += sizeof(val);
                n++;
            }
            #undef EMIT_PROP
            memcpy(count_field, &n, sizeof(n));

            send_data(cmd->code, cmd->transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_SEND_OBJECT_INFO: {
            uint8_t data[600];
            size_t got = 0;
            char filename[256];
            if (!read_small_data_phase(data, sizeof(data), &got) ||
                !parse_object_info_filename(data, got, filename, sizeof(filename))) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd->transaction_id, NULL, 0);
                break;
            }
            u32 compressed_size = 0;
            if (got >= 12) memcpy(&compressed_size, data + 8, 4);

            memset(&s_recv, 0, sizeof(s_recv));
            s_recv.active = true;
            snprintf(s_recv.filename, sizeof(s_recv.filename), "%s", filename);
            // 0xFFFFFFFF is the "doesn't fit in 32 bits" sentinel, not a
            // ~4GB file - taking it literally is what capped a >4GB
            // transfer at exactly "4.00 GB / 4.00 GB" before.
            s_recv.size_known = (compressed_size != 0xFFFFFFFFu);
            s_recv.declared_size = s_recv.size_known ? compressed_size : 0;
            snprintf(state->current_file, sizeof(state->current_file), "%s", filename);

            s_recv.handle = s_next_object_handle++;
            u32 resp_params[3] = { MTP_STORAGE_ID, 0 /* ParentObject - root */, s_recv.handle };
            send_response(PTP_RC_OK, cmd->transaction_id, resp_params, 3);
            break;
        }
        case PTP_OP_SEND_OBJECT_PROP_LIST: {
            // Params: StorageID, ParentObjectHandle, ObjectFormat, then
            // ObjectSize as a 64-bit value split high-word-first across the
            // last two slots - see read_param_u64_hi_lo on why the order
            // matters and where it was confirmed.
            uint64_t object_size = read_param_u64_hi_lo(params, params_len, 3);

            uint8_t data[600];
            size_t got = 0;
            char filename[256];
            uint64_t list_size = 0;
            bool list_size_known = false;
            if (!read_small_data_phase(data, sizeof(data), &got) ||
                !parse_object_prop_list(data, got, filename, sizeof(filename), &list_size, &list_size_known)) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd->transaction_id, NULL, 0);
                break;
            }

            memset(&s_recv, 0, sizeof(s_recv));
            s_recv.active = true;
            snprintf(s_recv.filename, sizeof(s_recv.filename), "%s", filename);
            // The property list's own ObjectSize wins when present - it and
            // the command parameters should agree, but the list is the more
            // specific statement of the two.
            if (list_size_known) {
                s_recv.declared_size = list_size;
                s_recv.size_known = true;
            } else if (object_size > 0) {
                s_recv.declared_size = object_size;
                s_recv.size_known = true;
            }
            snprintf(state->current_file, sizeof(state->current_file), "%s", filename);

            s_recv.handle = s_next_object_handle++;
            u32 resp_params[3] = { MTP_STORAGE_ID, 0 /* ParentObject - root */, s_recv.handle };
            send_response(PTP_RC_OK, cmd->transaction_id, resp_params, 3);
            break;
        }
        case PTP_OP_SEND_OBJECT: {
            // A completed object is still answerable for property queries,
            // but its data phase is over - re-sending it would mean
            // reinstalling on top of itself.
            if (!s_recv.active || s_recv.completed) {
                send_response(PTP_RC_GENERAL_ERROR, cmd->transaction_id, NULL, 0);
                break;
            }
            uint64_t expected_size = s_recv.declared_size;
            if (!open_sink_for_recv(s_recv.size_known ? expected_size : 0)) {
                send_response(PTP_RC_GENERAL_ERROR, cmd->transaction_id, NULL, 0);
                finish_recv(false, state, progress_cb, userdata);
                break;
            }

            bool received;
            if (s_recv.size_known) {
                received = receive_data_phase(&s_recv.sink, expected_size, expected_size, 0,
                                               progress_cb, userdata);
                if (received) s_recv.received = expected_size;
            } else {
                // No size to bound the read by (SendObjectInfo's sentinel,
                // and the host didn't use SendObjectPropList) - read until
                // the transfer itself ends. See
                // receive_data_phase_unbounded's doc comment.
                uint64_t total_received = 0;
                received = receive_data_phase_unbounded(&s_recv.sink, progress_cb, userdata, &total_received);
                if (received) s_recv.received = total_received;
            }

            // Install first, answer after. Committing blocks for seconds
            // without servicing USB, and a host that already has its OK
            // moves straight on to its next command - which then goes
            // unanswered for exactly that long, which is what Windows
            // reports as "the device stopped responding or was
            // disconnected". Holding the response until the object is
            // actually stored is also what SendObject's OK is supposed to
            // mean, so the host waits on the write it asked for instead of
            // on a command it thinks should have been instant.
            bool installed = finish_recv(received, state, progress_cb, userdata);
            send_response(installed ? PTP_RC_OK : PTP_RC_GENERAL_ERROR, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_ANDROID_GET_PARTIAL_OBJECT_64:
            // Read-back isn't something this responder ever needs - it's a
            // drop target (GetObjectHandles/GetObjectInfo above never
            // expose anything a host could meaningfully read back).
            send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
            break;
        case PTP_OP_ANDROID_BEGIN_EDIT_OBJECT: {
            u32 object_id = read_param_u32(params, params_len, 0);
            if (!s_recv.active || s_recv.completed || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            // A host only uses this sequence when it couldn't state the size
            // upfront, so 0 tells open_sink_for_recv the real size isn't
            // known yet - which for install_stream.h only affects the XCI
            // root-search window (falls back to uncapped, still correct).
            if (!open_sink_for_recv(s_recv.size_known ? s_recv.declared_size : 0)) {
                send_response(PTP_RC_GENERAL_ERROR, cmd->transaction_id, NULL, 0);
                finish_recv(false, state, progress_cb, userdata);
                break;
            }
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_ANDROID_SEND_PARTIAL_OBJECT: {
            // Parameter order (object_id, then size, then an offset
            // spanning two slots) matches haze's own read order exactly -
            // see read_param_u64's doc comment on why that's load-bearing
            // here rather than assumed from the wider Android/MTP spec.
            u32 object_id = read_param_u32(params, params_len, 0);
            u32 chunk_size = read_param_u32(params, params_len, 1);
            u64 offset = read_param_u64(params, params_len, 2);

            if (!s_recv.active || !s_recv.sink_open || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            // Every chunk has to land exactly where the last one left off -
            // install_stream_feed (and a plain staged fwrite) both assume a
            // strictly sequential stream, same as this whole responder's
            // transport does. A real MTP host writing a single file always
            // sends chunks in order in practice; if one ever didn't, failing
            // here beats silently writing content at the wrong offset.
            if (offset != s_recv.received) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd->transaction_id, NULL, 0);
                break;
            }

            // Best total available: what TruncateObject already stated, else
            // what SendObjectInfo/SendObjectPropList declared, else just
            // however far this chunk reaches (a growing estimate).
            uint64_t total = s_recv.truncated_size ? s_recv.truncated_size
                             : (s_recv.size_known ? s_recv.declared_size : s_recv.received + chunk_size);
            bool received = receive_data_phase(&s_recv.sink, chunk_size, total, s_recv.received,
                                                progress_cb, userdata);
            if (received) s_recv.received += chunk_size;

            if (!received) {
                send_response(PTP_RC_GENERAL_ERROR, cmd->transaction_id, NULL, 0);
                finish_recv(false, state, progress_cb, userdata);
                break;
            }
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_ANDROID_TRUNCATE_OBJECT: {
            u32 object_id = read_param_u32(params, params_len, 0);
            u64 size = read_param_u64(params, params_len, 1);
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            // Purely informational here - every content piece an NSP/XCI
            // actually installs is already individually sized from its own
            // PFS0/HFS0 entry (or its CNMT, for a compressed .ncz one), so
            // nothing downstream depends on the container's own overall
            // size. Kept only for GetObjectPropValue/GetObjectPropList to
            // report faithfully.
            s_recv.truncated_size = size;
            send_response(PTP_RC_OK, cmd->transaction_id, NULL, 0);
            break;
        }
        case PTP_OP_ANDROID_END_EDIT_OBJECT: {
            u32 object_id = read_param_u32(params, params_len, 0);
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd->transaction_id, NULL, 0);
                break;
            }
            // Same install-then-answer ordering as plain SendObject's - see
            // the comment there.
            bool installed = finish_recv(true, state, progress_cb, userdata);
            send_response(installed ? PTP_RC_OK : PTP_RC_GENERAL_ERROR, cmd->transaction_id, NULL, 0);
            break;
        }
        default:
            send_response(PTP_RC_OPERATION_NOT_SUPPORTED, cmd->transaction_id, NULL, 0);
            break;
    }
}

bool mtp_start(char *err_buf, size_t err_buf_size) {
    s_session_open = false;
    s_next_object_handle = 2;
    memset(&s_recv, 0, sizeof(s_recv));
    if (!mtp_usb_init(err_buf, err_buf_size)) return false;
    s_running = true;
    return true;
}

void mtp_stop(void) {
    mtp_usb_exit();
    s_running = false;
    s_session_open = false;
}

void mtp_step(MtpState *out, InstallProgressCallback progress_cb, void *userdata) {
    if (!s_running) {
        out->status = MTP_STATUS_WAITING_FOR_USB;
        return;
    }

    if (!mtp_usb_is_connected()) {
        out->status = MTP_STATUS_WAITING_FOR_USB;
        s_session_open = false;
        return;
    }
    out->status = s_session_open ? MTP_STATUS_IDLE : MTP_STATUS_WAITING_FOR_HOST;

    // Deliberately doesn't answer the USB Still Image class's EP0 control
    // requests (Get Device Status, Cancel, Device Reset) - a prior attempt
    // at answering Get Device Status here didn't fix the WPD/MTP driver
    // failure it was meant to address, and Atmosphère's own MTP homebrew
    // (troposphere/haze - a real, Windows-proven implementation) never
    // touches EP0 at all, only the bulk pipes below.

    // One read per container: a command's header and its (up to 5 u32)
    // parameters arrive together in a single transfer, so this reads a
    // whole packet and parses the header out of the front, keeping the rest
    // as the parameter bytes handle_command's individual cases read from.
    static uint8_t packet[512]; // one high-speed max packet; commands are at most 32 bytes
    int n = mtp_usb_read(packet, sizeof(packet), MTP_POLL_TIMEOUT_NS);
    if (n < (int)sizeof(PtpContainer)) return; // nothing arrived this poll, or a runt - next call tries again

    PtpContainer cmd;
    memcpy(&cmd, packet, sizeof(cmd));
    if (cmd.type != PTP_TYPE_COMMAND) return; // malformed - drop it

    handle_command(&cmd, packet + sizeof(cmd), (size_t)n - sizeof(cmd), out, progress_cb, userdata);
}
