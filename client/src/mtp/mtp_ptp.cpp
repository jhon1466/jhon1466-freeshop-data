#include "mtp_ptp.hpp"
#include "mtp_usb.h"
#include "install/package_stream.hpp"

#include <switch.h>

extern "C" {
#include "core/util.h"
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace pipensx::mtp {
namespace {

constexpr const char* kLandingDir = "sdmc:/switch/freeshop-client/mtp_incoming";
constexpr uint32_t kStorageId = 0x00010001u;

// Short poll while idle (no command in flight) - keeps mtp_step() cheap to
// call once per worker-thread iteration. Long timeout is for reads that are
// known to have more coming (mid data-phase) or where the host is expected
// to respond promptly (right after we've sent something) - a real MTP
// client doesn't sit idle mid-transaction, so a long wait there just means
// something's actually wrong (cable pulled, host hung).
constexpr uint64_t kPollTimeoutNs = 200000000ULL;
constexpr uint64_t kLongTimeoutNs = 10000000000ULL;

// Same on-wire layout as PTP's Generic Container header (PTP 1.0 / USB
// Still Image spec) - every command/data/response starts with exactly
// this, 12 bytes.
struct __attribute__((packed)) PtpContainer {
    uint32_t length;
    uint16_t type; // 1=Command, 2=Data, 3=Response, 4=Event
    uint16_t code;
    uint32_t transaction_id;
};
static_assert(sizeof(PtpContainer) == 12, "PtpContainer must be 12 bytes");

constexpr uint16_t PTP_TYPE_COMMAND = 1;
constexpr uint16_t PTP_TYPE_DATA = 2;
constexpr uint16_t PTP_TYPE_RESPONSE = 3;

constexpr uint16_t PTP_OP_GET_DEVICE_INFO = 0x1001;
constexpr uint16_t PTP_OP_OPEN_SESSION = 0x1002;
constexpr uint16_t PTP_OP_CLOSE_SESSION = 0x1003;
constexpr uint16_t PTP_OP_GET_STORAGE_IDS = 0x1004;
constexpr uint16_t PTP_OP_GET_STORAGE_INFO = 0x1005;
constexpr uint16_t PTP_OP_GET_OBJECT_HANDLES = 0x1007;
constexpr uint16_t PTP_OP_GET_OBJECT_INFO = 0x1008;
constexpr uint16_t PTP_OP_DELETE_OBJECT = 0x100B;
constexpr uint16_t PTP_OP_SEND_OBJECT_INFO = 0x100C;
constexpr uint16_t PTP_OP_SEND_OBJECT = 0x100D;
// MTP object-properties operations - Windows uses GetObjectPropDesc on
// ObjectSize specifically to check whether this device reports sizes as a
// 64-bit value (see PTP_PROP_OBJECT_SIZE's handling below) before it will
// attempt pushing a file over 4GB at all - without this, and the Android
// operations below, Windows silently refuses/truncates any transfer that
// big.
constexpr uint16_t PTP_OP_GET_OBJECT_PROPS_SUPPORTED = 0x9801;
constexpr uint16_t PTP_OP_GET_OBJECT_PROP_DESC = 0x9802;
constexpr uint16_t PTP_OP_GET_OBJECT_PROP_VALUE = 0x9803;
constexpr uint16_t PTP_OP_SET_OBJECT_PROP_VALUE = 0x9804;
constexpr uint16_t PTP_OP_GET_OBJECT_PROP_LIST = 0x9805;
// SendObjectInfo's replacement for anything over 4GB, and the only way this
// responder ever learns such a file's real size: its ObjectSize travels as a
// 64-bit value in the *command parameters* (see the handler for the exact
// slot order), not in a 32-bit dataset field. A host only uses it if the
// device lists it here - without it Windows falls back to SendObjectInfo
// with a 0xFFFFFFFF "can't tell you" size, which is why a >4GB transfer used
// to run with no total to show progress against.
constexpr uint16_t PTP_OP_SEND_OBJECT_PROP_LIST = 0x9808;
// Android's MTP large-object extension - the actual >4GB transfer mechanism:
// SendObjectInfo's own ObjectCompressedSize field is 32-bit, so a host that
// knows a file exceeds that sends 0xFFFFFFFF there instead and pushes the
// real data through this sequence rather than plain SendObject.
constexpr uint16_t PTP_OP_ANDROID_GET_PARTIAL_OBJECT_64 = 0x95C1;
constexpr uint16_t PTP_OP_ANDROID_SEND_PARTIAL_OBJECT = 0x95C2;
constexpr uint16_t PTP_OP_ANDROID_TRUNCATE_OBJECT = 0x95C3;
constexpr uint16_t PTP_OP_ANDROID_BEGIN_EDIT_OBJECT = 0x95C4;
constexpr uint16_t PTP_OP_ANDROID_END_EDIT_OBJECT = 0x95C5;

constexpr uint16_t PTP_RC_OK = 0x2001;
constexpr uint16_t PTP_RC_GENERAL_ERROR = 0x2002;
constexpr uint16_t PTP_RC_OPERATION_NOT_SUPPORTED = 0x2005;
constexpr uint16_t PTP_RC_INVALID_OBJECT_HANDLE = 0x2009;
constexpr uint16_t PTP_RC_INVALID_PARAMETER = 0x201D;
constexpr uint16_t PTP_RC_INVALID_OBJECT_PROP_CODE = 0xA801; // MTP extension

// MTP object property codes - the subset haze itself declares, which is
// what real-hardware testing against Windows is calibrated against.
constexpr uint16_t PTP_PROP_STORAGE_ID = 0xDC01;
constexpr uint16_t PTP_PROP_OBJECT_FORMAT = 0xDC02;
constexpr uint16_t PTP_PROP_OBJECT_SIZE = 0xDC04;
constexpr uint16_t PTP_PROP_OBJECT_FILENAME = 0xDC07;
constexpr uint16_t PTP_PROP_PARENT_OBJECT = 0xDC0B;
constexpr uint16_t PTP_PROP_PERSISTENT_UID = 0xDC41;

// PTP property dataset type codes (PTP 1.0 spec Annex).
constexpr uint16_t PTP_DTC_U16 = 0x0004;
constexpr uint16_t PTP_DTC_U32 = 0x0006;
constexpr uint16_t PTP_DTC_U64 = 0x0008;
constexpr uint16_t PTP_DTC_U128 = 0x000A;
constexpr uint16_t PTP_DTC_STRING = 0xFFFF;
constexpr uint8_t PTP_PROP_GETSET_GET = 0x00;
constexpr uint8_t PTP_PROP_GETSET_GETSET = 0x01;

bool s_running = false;
bool s_session_open = false;
uint32_t s_next_object_handle = 2; // 1 is never used - keeps 0/1 unambiguous as "no handle yet"
std::string s_working_root;
install::InstallStorageTarget s_target = install::InstallStorageTarget::SdCard;

bool mkdirIgnoreExists(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

// Where the SendObject/SendPartialObject data phase actually goes depends
// on the file. A .nsp/.nsz installs straight into the streaming installer
// (install::PackageStream + install::InstallBackend) as it arrives; anything
// else is staged to a file, the payload never buffered whole in memory
// either way.
struct ObjectSink {
    std::unique_ptr<install::InstallBackend> backend; // non-null for a direct install
    std::unique_ptr<install::PackageStream> stream;    // non-null for a direct install
    FILE* fp = nullptr;                                 // non-null for a staged file
    std::string err;
};

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
struct PendingReceive {
    bool active = false;   // SendObjectInfo ran, this slot describes a real object (in flight or finished)
    bool sink_open = false; // the receive sink itself has been opened (SendObject, or BeginEditObject)
    // Set once the object has been fully received and installed/staged. The
    // record deliberately stays `active` afterwards: a host that has just
    // finished writing a file immediately asks about it again
    // (GetObjectInfo, the *ObjectProp* operations) to refresh its own view,
    // and answering "invalid handle" to those is read as the copy having
    // failed.
    bool completed = false;
    std::string filename;
    uint32_t handle = 0;
    // The object's real size, and whether it's actually known.
    // SendObjectInfo can only carry 32 bits and sends 0xFFFFFFFF when the
    // truth doesn't fit (leaving this unknown); SendObjectPropList carries
    // a real 64-bit value.
    uint64_t declared_size = 0;
    bool size_known = false;
    uint64_t truncated_size = 0; // set by TruncateObject once known, 0 until then
    uint64_t received = 0;       // bytes fed into sink so far
    std::string dest_path;       // only meaningful for a staged (unsupported-format) sink
    ObjectSink sink;
};
PendingReceive s_recv;

// ---- Byte-packing helpers for PTP datasets - every field is little-endian,
// matching ARM64, so these are just typed memcpys with a cursor. ----
void put_u16(uint8_t** p, uint16_t v) { memcpy(*p, &v, 2); *p += 2; }
void put_u32(uint8_t** p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
void put_u64(uint8_t** p, uint64_t v) { memcpy(*p, &v, 8); *p += 8; }
void put_u8(uint8_t** p, uint8_t v) { **p = v; *p += 1; }

// PTP String: a 1-byte character count *including* the terminating NUL,
// then that many UTF-16LE code units (count=0 means empty, no data
// follows). Every string this responder sends is a plain ASCII literal, so
// this widens each byte to UTF-16 rather than needing a real UTF-8 decoder.
void put_string(uint8_t** p, const char* ascii) {
    size_t len = strlen(ascii);
    if (len > 254) len = 254; // count byte is 1 byte (max 255) - leave room for the NUL
    if (len == 0) { **p = 0; *p += 1; return; }
    **p = (uint8_t)(len + 1);
    *p += 1;
    for (size_t i = 0; i < len; i++) put_u16(p, (uint16_t)(unsigned char)ascii[i]);
    put_u16(p, 0);
}

void put_u16_array(uint8_t** p, const uint16_t* items, uint32_t count) {
    put_u32(p, count);
    for (uint32_t i = 0; i < count; i++) put_u16(p, items[i]);
}

// Reads command parameter slot `index` (0-based, 4 bytes each) as a u32.
// Returns 0 if the packet was too short to contain it - every caller here
// treats a too-short param list as "parameter is 0/absent" rather than a
// transport error, matching how little these parameters are actually
// trusted (object_id is always cross-checked against s_recv.handle; a
// wrong-but-present-looking 0 just fails that check like any other mismatch).
uint32_t read_param_u32(const uint8_t* params, size_t params_len, int index) {
    size_t off = (size_t)index * 4;
    if (off + 4 > params_len) return 0;
    uint32_t v;
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
uint64_t read_param_u64(const uint8_t* params, size_t params_len, int index) {
    size_t off = (size_t)index * 4;
    if (off + 8 > params_len) return 0;
    uint64_t v;
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
uint64_t read_param_u64_hi_lo(const uint8_t* params, size_t params_len, int hi_index) {
    size_t off = (size_t)hi_index * 4;
    if (off + 8 > params_len) return 0;
    uint32_t hi, lo;
    memcpy(&hi, params + off, 4);
    memcpy(&lo, params + off + 4, 4);
    return ((uint64_t)hi << 32) | lo;
}

// ---- Container I/O ----

bool send_response(uint16_t code, uint32_t transaction_id, const uint32_t* params, int nparams) {
    uint8_t buf[12 + 5 * 4];
    PtpContainer hdr = { (uint32_t)(12 + nparams * 4), PTP_TYPE_RESPONSE, code, transaction_id };
    memcpy(buf, &hdr, 12);
    for (int i = 0; i < nparams; i++) memcpy(buf + 12 + i * 4, &params[i], 4);
    return mtp_usb_write(buf, 12 + (size_t)nparams * 4, kLongTimeoutNs) == 12 + nparams * 4;
}

// Header and body go out as one transfer, not two. A PTP container is one
// logical unit on the wire, and splitting it would put a short packet
// (12 bytes, under the 512-byte endpoint max) between the two halves -
// which is exactly how USB signals "this transfer is over", so a host
// would see a truncated container followed by a stray one.
bool send_data(uint16_t code, uint32_t transaction_id, const uint8_t* data, size_t len) {
    static uint8_t packet[1024];
    if (12 + len > sizeof(packet)) return false;

    PtpContainer hdr = { (uint32_t)(12 + len), PTP_TYPE_DATA, code, transaction_id };
    memcpy(packet, &hdr, 12);
    if (len > 0) memcpy(packet + 12, data, len);
    return mtp_usb_write(packet, 12 + len, kLongTimeoutNs) == (int)(12 + len);
}

// Reads one small Data-phase container in one shot - used for
// SendObjectInfo's ObjectInfo dataset (well under a KB, never a large file
// payload, which streams separately in receive_data_phase). The container
// header arrives in the same transfer as the body it describes, so both
// come out of a single read.
bool read_small_data_phase(uint8_t* buf, size_t buf_cap, size_t* out_len) {
    static uint8_t packet[4096];
    int n = mtp_usb_read(packet, sizeof(packet), kLongTimeoutNs);
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
void drain_data_phase() {
    static uint8_t discard[4096];
    mtp_usb_read(discard, sizeof(discard), kLongTimeoutNs);
}

// ---- GetDeviceInfo ----

const uint16_t kOperationsSupported[] = {
    PTP_OP_GET_DEVICE_INFO, PTP_OP_OPEN_SESSION, PTP_OP_CLOSE_SESSION, PTP_OP_GET_STORAGE_IDS,
    PTP_OP_GET_STORAGE_INFO, PTP_OP_GET_OBJECT_HANDLES, PTP_OP_GET_OBJECT_INFO, PTP_OP_DELETE_OBJECT,
    PTP_OP_SEND_OBJECT_INFO, PTP_OP_SEND_OBJECT,
    PTP_OP_GET_OBJECT_PROPS_SUPPORTED, PTP_OP_GET_OBJECT_PROP_DESC, PTP_OP_GET_OBJECT_PROP_VALUE,
    PTP_OP_SET_OBJECT_PROP_VALUE, PTP_OP_GET_OBJECT_PROP_LIST, PTP_OP_SEND_OBJECT_PROP_LIST,
    PTP_OP_ANDROID_GET_PARTIAL_OBJECT_64, PTP_OP_ANDROID_SEND_PARTIAL_OBJECT, PTP_OP_ANDROID_TRUNCATE_OBJECT,
    PTP_OP_ANDROID_BEGIN_EDIT_OBJECT, PTP_OP_ANDROID_END_EDIT_OBJECT,
};
const uint16_t kImageFormats[] = { 0x3000, 0x3001 }; // Undefined (any binary file), Association (folder)
const uint16_t kObjectPropsSupported[] = {
    PTP_PROP_STORAGE_ID, PTP_PROP_OBJECT_FORMAT, PTP_PROP_OBJECT_SIZE,
    PTP_PROP_OBJECT_FILENAME, PTP_PROP_PARENT_OBJECT, PTP_PROP_PERSISTENT_UID,
};

size_t build_device_info(uint8_t* buf) {
    uint8_t* p = buf;
    put_u16(&p, 100);                          // StandardVersion
    put_u32(&p, 6);                            // VendorExtensionID - Microsoft, the MTP signature within plain PTP
    put_u16(&p, 100);                          // VendorExtensionVersion
    put_string(&p, "microsoft.com: 1.0");      // VendorExtensionDesc - what actually marks this as MTP, not just PTP
    put_u16(&p, 0);                            // FunctionalMode
    put_u16_array(&p, kOperationsSupported, sizeof(kOperationsSupported) / sizeof(uint16_t));
    put_u16_array(&p, nullptr, 0);             // EventsSupported - none, this responder never pushes async events
    put_u16_array(&p, nullptr, 0);             // DevicePropertiesSupported - none
    put_u16_array(&p, nullptr, 0);             // CaptureFormats - none, this isn't a camera
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

bool parse_object_info_filename(const uint8_t* data, size_t len, char* out_name, size_t out_size) {
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
bool prop_value_size(uint16_t datatype, const uint8_t* data, size_t avail, size_t* out_size) {
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
        uint32_t count;
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
bool parse_object_prop_list(const uint8_t* data, size_t len, char* out_name, size_t out_name_size,
                             uint64_t* out_size, bool* out_size_known) {
    if (len < 4) return false;
    uint32_t count;
    memcpy(&count, data, 4);

    size_t off = 4;
    bool got_name = false;

    for (uint32_t i = 0; i < count; i++) {
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
size_t build_object_info(uint8_t* buf, uint32_t size, const char* filename) {
    uint8_t* p = buf;
    put_u32(&p, kStorageId);
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

bool sink_write(ObjectSink& sink, const uint8_t* data, size_t len) {
    if (len == 0) return true;
    if (sink.stream) return sink.stream->write(data, len);
    return fwrite(data, 1, len, sink.fp) == len;
}

// Reads exactly one Data-phase container (header + up to `expected_size`
// bytes of payload) into `sink`, reporting progress through `progressCb` as
// `(total, done)`. Used both by plain SendObject (one call, the whole
// object) and by each individual SendPartialObject call (one call per
// chunk of a large object) - the caller decides what `expected_size`,
// `total` and `done`'s starting point mean for its own case.
bool receive_data_phase(ObjectSink& sink, uint64_t expected_size, uint64_t progress_total,
                        uint64_t progress_base, const MtpProgressCallback& progressCb) {
    // The Data container's 12-byte header shares its transfer with the
    // first chunk of payload - so this first read has to keep what follows
    // the header rather than discarding the rest of the packet.
    static uint8_t buf[256 * 1024];
    int first = mtp_usb_read(buf, sizeof(buf), kLongTimeoutNs);
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
        if (ok && progressCb && !progressCb(progress_total, progress_base + done)) ok = false;
    }

    while (ok && done < expected_size) {
        uint64_t remaining = expected_size - done;
        size_t want = remaining < (uint64_t)sizeof(buf) ? (size_t)remaining : sizeof(buf);
        int got = mtp_usb_read(buf, want, kLongTimeoutNs);
        if (got <= 0) { ok = false; break; }
        if (!sink_write(sink, buf, (size_t)got)) { ok = false; break; }
        done += (uint64_t)got;
        if (progressCb && !progressCb(progress_total, progress_base + done)) { ok = false; break; }
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
// literally. Treating 0xFFFFFFFF as a real 4-ish-GB byte count is exactly
// why a >4GB XCI would cut off at "4.00 GB / 4.00 GB" if this ever got that
// simplified - that ceiling would be this responder's own doing, not
// anything Windows or the transport actually stops at.
//
// A USB bulk transfer's end is unambiguous without any pre-known length:
// the sender's last packet is short (fewer bytes than the endpoint's max
// packet size), or - if the true length happens to be an exact multiple of
// that - followed by an explicit zero-length packet. Either one ends this
// loop; nothing here needs to know the real size in advance.
bool receive_data_phase_unbounded(ObjectSink& sink, const MtpProgressCallback& progressCb,
                                   uint64_t* out_received) {
    static uint8_t buf[256 * 1024];
    int first = mtp_usb_read(buf, sizeof(buf), kLongTimeoutNs);
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
        // total=0 here (unknown) - the UI's progress callback already falls
        // back to a plain byte count instead of a percentage when told the
        // total isn't known, which is exactly right for this case.
        if (ok && progressCb && !progressCb(0, done)) ok = false;
    }

    while (ok && more_may_follow) {
        int got = mtp_usb_read(buf, sizeof(buf), kLongTimeoutNs);
        if (got < 0) { ok = false; break; }
        if (got == 0) break; // explicit zero-length packet - clean end of transfer
        if (!sink_write(sink, buf, (size_t)got)) { ok = false; break; }
        done += (uint64_t)got;
        if (progressCb && !progressCb(0, done)) { ok = false; break; }
        more_may_follow = ((size_t)got == sizeof(buf)); // short packet => that was the last one
    }

    if (ok && out_received) *out_received = done;
    return ok;
}

// ---- Shared sink lifecycle - used by both the plain SendObject path and
// the Android BeginEditObject/SendPartialObject/.../EndEditObject one. ----

// Opens s_recv.sink for s_recv.filename: the streaming installer for a
// .nsp/.nsz/.xci/.xcz, a staged file for anything else. `size_hint` is
// currently unused by PackageStream but kept for symmetry with the rest of
// the receive path.
bool open_sink_for_recv(uint64_t size_hint) {
    (void)size_hint;
    const std::string& filename = s_recv.filename;
    const size_t dot = filename.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : filename.substr(dot);
    auto ciEquals = [](const std::string& a, const char* b) {
        return strcasecmp(a.c_str(), b) == 0;
    };
    const bool is_nsp = ciEquals(ext, ".nsp") || ciEquals(ext, ".nsz");
    const bool is_xci = ciEquals(ext, ".xci") || ciEquals(ext, ".xcz");

    s_recv.sink = ObjectSink{};
    s_recv.dest_path.clear();

    if (is_nsp || is_xci) {
        s_recv.sink.backend = install::createInstallBackend(s_working_root, s_target);
        char taskId[48];
        snprintf(taskId, sizeof(taskId), "mtp-%08x", s_recv.handle);
        if (!s_recv.sink.backend->beginPackage(taskId, filename)) {
            s_recv.sink.err = s_recv.sink.backend->error();
            s_recv.sink.backend.reset();
            return false;
        }
        install::InstallBackend* backend = s_recv.sink.backend.get();
        install::PackageCallbacks callbacks;
        callbacks.beginFile = [backend](const std::string& name, uint64_t size) {
            return backend->beginFile(name, size);
        };
        callbacks.setFileSize = [backend](uint64_t size) {
            return backend->setFileSize(size);
        };
        callbacks.writeFile = [backend](const uint8_t* data, size_t size) {
            return backend->writeFile(data, size);
        };
        callbacks.endFile = [backend] { return backend->endFile(); };
        const bool compressed = ciEquals(ext, ".nsz") || ciEquals(ext, ".xcz");
        s_recv.sink.stream = std::make_unique<install::PackageStream>(
            compressed, std::move(callbacks), taskId, is_xci);
    } else {
        mkdirIgnoreExists("sdmc:/switch/freeshop-client");
        mkdirIgnoreExists(kLandingDir);
        s_recv.dest_path = std::string(kLandingDir) + "/" + filename;
        s_recv.sink.fp = fopen(s_recv.dest_path.c_str(), "wb");
        if (!s_recv.sink.fp) {
            s_recv.sink.err = "no se pudo crear " + s_recv.dest_path;
            return false;
        }
    }
    s_recv.sink_open = true;
    s_recv.received = 0;
    return true;
}

// Records one finished transfer - dropping the oldest entry first if the
// history is already full, so this always reflects the most *recent*
// completions rather than silently refusing new ones past kMtpHistoryMax.
void push_history(MtpState& state, const std::string& filename, MtpHistoryStatus status,
                  const std::string& error) {
    if (state.history.size() >= kMtpHistoryMax)
        state.history.erase(state.history.begin());
    state.history.push_back(MtpHistoryItem{filename, status, error});
}

// Closes s_recv.sink and, on a successful receive, runs the actual
// install - recording the outcome in `state`'s history either way. Used
// both when plain SendObject finishes and when EndEditObject closes out a
// large-object upload. Always clears s_recv.active/sink_open. Returns
// whether the object ended up installed, so the caller can answer the
// host accordingly.
bool finish_recv(bool received_ok, MtpState& state, const MtpProgressCallback& progressCb) {
    ObjectSink& sink = s_recv.sink;
    if (sink.fp) { fclose(sink.fp); sink.fp = nullptr; }

    if (!received_ok) {
        if (sink.stream) sink.stream.reset();
        if (sink.backend) { sink.backend->rollbackPackage(); sink.backend.reset(); }
        else if (!s_recv.dest_path.empty()) remove(s_recv.dest_path.c_str());
        push_history(state, s_recv.filename, MtpHistoryStatus::Failed,
                    !sink.err.empty() ? sink.err : "recepción interrumpida");
        s_recv.active = false;
        s_recv.sink_open = false;
        state.currentFile.clear();
        return false;
    }

    bool installed = false;
    // Whatever the host actually delivered - reported back from here on, so
    // post-transfer property queries state the real size rather than a
    // declaration that may have been the "doesn't fit in 32 bits" sentinel.
    s_recv.declared_size = s_recv.received;
    s_recv.size_known = true;

    if (sink.stream) {
        // Committing takes a few seconds (reading the CNMT back, writing the
        // meta record, importing tickets) with no USB serviced in between -
        // let the UI say so rather than leaving a finished progress bar on
        // screen looking hung.
        state.status = MtpStatus::Installing;
        if (progressCb) progressCb(0, 0);

        bool finishedOk = sink.stream->finish();
        std::string err = sink.stream->error();
        sink.stream.reset();
        bool alreadyInstalled = false;
        if (finishedOk && sink.backend->commitPackage(alreadyInstalled)) {
            push_history(state, s_recv.filename, MtpHistoryStatus::Installed, "");
            installed = true;
        } else {
            if (err.empty()) err = sink.backend->error();
            sink.backend->rollbackPackage();
            push_history(state, s_recv.filename, MtpHistoryStatus::Failed, err);
        }
        sink.backend.reset();
    } else {
        push_history(state, s_recv.filename, MtpHistoryStatus::Failed,
                    "recibido pero no instalado (MTP solo instala directo "
                    "NSP/NSZ/XCI/XCZ)");
    }

    // The object exists from the host's point of view either way (it was
    // received in full; only installing it may have failed), so the handle
    // stays answerable - see PendingReceive::completed.
    s_recv.completed = true;
    s_recv.sink_open = false;
    state.currentFile.clear();
    return installed;
}

// ---- Command dispatch ----

void handle_command(const PtpContainer& cmd, const uint8_t* params, size_t params_len, MtpState& state,
                    const MtpProgressCallback& progressCb) {
    switch (cmd.code) {
        case PTP_OP_GET_DEVICE_INFO: {
            static uint8_t buf[512];
            size_t len = build_device_info(buf);
            send_data(cmd.code, cmd.transaction_id, buf, len);
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_OPEN_SESSION:
            s_session_open = true;
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        case PTP_OP_CLOSE_SESSION:
            s_session_open = false;
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        case PTP_OP_GET_STORAGE_IDS: {
            uint8_t buf[8];
            uint8_t* p = buf;
            put_u32(&p, 1);
            put_u32(&p, kStorageId);
            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_GET_STORAGE_INFO: {
            // Real SD card numbers, same statvfs the installers use for
            // their own space checks - Windows shows these on the device's
            // drive entry, and hardcoded placeholders there are actively
            // misleading (a "64 GB free of 128 GB" that matches no real
            // card, and would let Explorer start a copy that can't fit).
            uint64_t total_bytes = 0, free_bytes = 0;
            struct statvfs st;
            if (statvfs("sdmc:/", &st) == 0) {
                total_bytes = (uint64_t)st.f_frsize * st.f_blocks;
                free_bytes = (uint64_t)st.f_bsize * st.f_bavail;
            }

            uint8_t buf[96];
            uint8_t* p = buf;
            put_u16(&p, 0x0004);       // StorageType - Fixed RAM (closest fit for "just works")
            put_u16(&p, 0x0002);       // FilesystemType - Generic Hierarchical
            put_u16(&p, 0x0000);       // AccessCapability - ReadWrite
            put_u64(&p, total_bytes);  // MaxCapacity
            put_u64(&p, free_bytes);   // FreeSpaceInBytes
            put_u32(&p, 0xFFFFFFFF);   // FreeSpaceInObjects - not tracked
            put_string(&p, "Tarjeta SD"); // StorageDescription
            put_string(&p, "");           // VolumeLabel
            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_HANDLES: {
            // Always empty - this responder is a drop target, not a file
            // browser. A real MTP client still expects a (possibly
            // zero-length) ObjectHandle array back, not an error, when
            // asked to list a valid storage's root.
            uint8_t buf[4];
            uint8_t* p = buf;
            put_u32(&p, 0);
            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_INFO: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            // GetObjectHandles above never reports a handle, so the only
            // one a host can meaningfully ask about is the one this
            // responder itself just handed out via SendObjectInfo.
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            // ObjectInfo's own size field is 32-bit, so anything that
            // doesn't fit goes back out as the same 0xFFFFFFFF sentinel a
            // host would have sent us - its real value is only ever
            // readable through PTP_PROP_OBJECT_SIZE (64-bit).
            uint64_t real_size = s_recv.size_known ? s_recv.declared_size : s_recv.received;
            uint32_t size = real_size > 0xFFFFFFFEu ? 0xFFFFFFFFu : (uint32_t)real_size;
            static uint8_t buf[600];
            size_t len = build_object_info(buf, size, s_recv.filename.c_str());
            send_data(cmd.code, cmd.transaction_id, buf, len);
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_DELETE_OBJECT: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            // Only ever our own currently-pending object - a host cancelling
            // a copy mid-transfer deletes the partial object it just
            // created, which this responder needs to clean up (an
            // abandoned install or staged file otherwise). Nothing else is
            // ever a valid handle here (see GetObjectInfo above).
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            // An already-installed object can't be undone from here (that
            // would mean uninstalling a title the user now has), so the
            // handle is simply retired - answering OK without touching the
            // install, rather than failing a cleanup the host considers
            // routine.
            if (s_recv.sink_open) {
                if (s_recv.sink.fp) { fclose(s_recv.sink.fp); s_recv.sink.fp = nullptr; }
                if (s_recv.sink.stream) s_recv.sink.stream.reset();
                if (s_recv.sink.backend) { s_recv.sink.backend->rollbackPackage(); s_recv.sink.backend.reset(); }
                else if (!s_recv.dest_path.empty()) remove(s_recv.dest_path.c_str());
            }
            s_recv.active = false;
            s_recv.sink_open = false;
            s_recv.completed = false;
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_PROPS_SUPPORTED: {
            uint8_t buf[4 + sizeof(kObjectPropsSupported)];
            uint8_t* p = buf;
            put_u16_array(&p, kObjectPropsSupported, sizeof(kObjectPropsSupported) / sizeof(uint16_t));
            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_PROP_DESC: {
            uint32_t property_code = read_param_u32(params, params_len, 0);
            uint8_t buf[48];
            uint8_t* p = buf;
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
                    send_response(PTP_RC_INVALID_OBJECT_PROP_CODE, cmd.transaction_id, nullptr, 0);
                    return;
            }
            put_u32(&p, 0); // GroupCode - unused, required by the dataset shape
            put_u8(&p, 0);  // FormFlag - None
            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_GET_OBJECT_PROP_VALUE: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            uint32_t property_code = read_param_u32(params, params_len, 1);
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            uint8_t buf[300];
            uint8_t* p = buf;
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
                case PTP_PROP_STORAGE_ID: put_u32(&p, kStorageId); break;
                case PTP_PROP_PARENT_OBJECT: put_u32(&p, 0); break;
                case PTP_PROP_OBJECT_FORMAT: put_u16(&p, 0x3000); break;
                case PTP_PROP_OBJECT_FILENAME: put_string(&p, s_recv.filename.c_str()); break;
                default:
                    send_response(PTP_RC_INVALID_OBJECT_PROP_CODE, cmd.transaction_id, nullptr, 0);
                    return;
            }
            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_SET_OBJECT_PROP_VALUE:
            // Renaming a pending object isn't something installing over
            // MTP needs - reject cleanly, but the data phase the host
            // already committed to sending still has to be drained so the
            // next command's read doesn't consume its tail instead.
            drain_data_phase();
            send_response(PTP_RC_OPERATION_NOT_SUPPORTED, cmd.transaction_id, nullptr, 0);
            break;
        case PTP_OP_GET_OBJECT_PROP_LIST: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            uint32_t object_format = read_param_u32(params, params_len, 1);
            int32_t property_code = (int32_t)read_param_u32(params, params_len, 2);
            int32_t group_code = (int32_t)read_param_u32(params, params_len, 3);
            int32_t depth = (int32_t)read_param_u32(params, params_len, 4);

            if (object_format != 0 || group_code != 0 || depth != 0) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd.transaction_id, nullptr, 0);
                break;
            }
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            if (property_code != -1 && property_code != PTP_PROP_STORAGE_ID &&
                property_code != PTP_PROP_OBJECT_FORMAT && property_code != PTP_PROP_OBJECT_SIZE &&
                property_code != PTP_PROP_OBJECT_FILENAME && property_code != PTP_PROP_PARENT_OBJECT &&
                property_code != PTP_PROP_PERSISTENT_UID) {
                send_response(PTP_RC_INVALID_OBJECT_PROP_CODE, cmd.transaction_id, nullptr, 0);
                break;
            }

            uint64_t size = s_recv.truncated_size ? s_recv.truncated_size
                             : (s_recv.size_known ? s_recv.declared_size : s_recv.received);

            static uint8_t buf[600];
            uint8_t* p = buf;
            uint8_t* count_field = p;
            put_u32(&p, 0); // filled in below, once the real count is known
            uint32_t n = 0;

            #define EMIT_PROP(code) (property_code == -1 || property_code == (code))
            if (EMIT_PROP(PTP_PROP_STORAGE_ID)) {
                put_u32(&p, object_id); put_u16(&p, PTP_PROP_STORAGE_ID); put_u16(&p, PTP_DTC_U32);
                put_u32(&p, kStorageId); n++;
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
                put_string(&p, s_recv.filename.c_str()); n++;
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

            send_data(cmd.code, cmd.transaction_id, buf, (size_t)(p - buf));
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_SEND_OBJECT_INFO: {
            uint8_t data[600];
            size_t got = 0;
            char filename[256];
            if (!read_small_data_phase(data, sizeof(data), &got) ||
                !parse_object_info_filename(data, got, filename, sizeof(filename))) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd.transaction_id, nullptr, 0);
                break;
            }
            uint32_t compressed_size = 0;
            if (got >= 12) memcpy(&compressed_size, data + 8, 4);

            s_recv = PendingReceive{};
            s_recv.active = true;
            s_recv.filename = filename;
            // 0xFFFFFFFF is the "doesn't fit in 32 bits" sentinel, not a
            // ~4GB file - taking it literally is what would cap a >4GB
            // transfer at exactly "4.00 GB / 4.00 GB".
            s_recv.size_known = (compressed_size != 0xFFFFFFFFu);
            s_recv.declared_size = s_recv.size_known ? compressed_size : 0;
            state.currentFile = filename;

            s_recv.handle = s_next_object_handle++;
            uint32_t resp_params[3] = { kStorageId, 0 /* ParentObject - root */, s_recv.handle };
            send_response(PTP_RC_OK, cmd.transaction_id, resp_params, 3);
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
                send_response(PTP_RC_INVALID_PARAMETER, cmd.transaction_id, nullptr, 0);
                break;
            }

            s_recv = PendingReceive{};
            s_recv.active = true;
            s_recv.filename = filename;
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
            state.currentFile = filename;

            s_recv.handle = s_next_object_handle++;
            uint32_t resp_params[3] = { kStorageId, 0 /* ParentObject - root */, s_recv.handle };
            send_response(PTP_RC_OK, cmd.transaction_id, resp_params, 3);
            break;
        }
        case PTP_OP_SEND_OBJECT: {
            // A completed object is still answerable for property queries,
            // but its data phase is over - re-sending it would mean
            // reinstalling on top of itself.
            if (!s_recv.active || s_recv.completed) {
                send_response(PTP_RC_GENERAL_ERROR, cmd.transaction_id, nullptr, 0);
                break;
            }
            uint64_t expected_size = s_recv.declared_size;
            if (!open_sink_for_recv(s_recv.size_known ? expected_size : 0)) {
                send_response(PTP_RC_GENERAL_ERROR, cmd.transaction_id, nullptr, 0);
                finish_recv(false, state, progressCb);
                break;
            }

            bool received;
            if (s_recv.size_known) {
                received = receive_data_phase(s_recv.sink, expected_size, expected_size, 0, progressCb);
                if (received) s_recv.received = expected_size;
            } else {
                // No size to bound the read by (SendObjectInfo's sentinel,
                // and the host didn't use SendObjectPropList) - read until
                // the transfer itself ends. See
                // receive_data_phase_unbounded's doc comment.
                uint64_t total_received = 0;
                received = receive_data_phase_unbounded(s_recv.sink, progressCb, &total_received);
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
            bool installed = finish_recv(received, state, progressCb);
            send_response(installed ? PTP_RC_OK : PTP_RC_GENERAL_ERROR, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_ANDROID_GET_PARTIAL_OBJECT_64:
            // Read-back isn't something this responder ever needs - it's a
            // drop target (GetObjectHandles/GetObjectInfo above never
            // expose anything a host could meaningfully read back).
            send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
            break;
        case PTP_OP_ANDROID_BEGIN_EDIT_OBJECT: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            if (!s_recv.active || s_recv.completed || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            // A host only uses this sequence when it couldn't state the size
            // upfront - open_sink_for_recv doesn't currently need it either
            // way (PackageStream discovers structure from the bytes
            // themselves).
            if (!open_sink_for_recv(s_recv.size_known ? s_recv.declared_size : 0)) {
                send_response(PTP_RC_GENERAL_ERROR, cmd.transaction_id, nullptr, 0);
                finish_recv(false, state, progressCb);
                break;
            }
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_ANDROID_SEND_PARTIAL_OBJECT: {
            // Parameter order (object_id, then size, then an offset
            // spanning two slots) matches haze's own read order exactly -
            // see read_param_u64's doc comment on why that's load-bearing
            // here rather than assumed from the wider Android/MTP spec.
            uint32_t object_id = read_param_u32(params, params_len, 0);
            uint32_t chunk_size = read_param_u32(params, params_len, 1);
            uint64_t offset = read_param_u64(params, params_len, 2);

            if (!s_recv.active || !s_recv.sink_open || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            // Every chunk has to land exactly where the last one left off -
            // both PackageStream and a plain staged fwrite assume a
            // strictly sequential stream, same as this whole responder's
            // transport does. A real MTP host writing a single file always
            // sends chunks in order in practice; if one ever didn't, failing
            // here beats silently writing content at the wrong offset.
            if (offset != s_recv.received) {
                send_response(PTP_RC_INVALID_PARAMETER, cmd.transaction_id, nullptr, 0);
                break;
            }

            // Best total available: what TruncateObject already stated, else
            // what SendObjectInfo/SendObjectPropList declared, else just
            // however far this chunk reaches (a growing estimate).
            uint64_t total = s_recv.truncated_size ? s_recv.truncated_size
                             : (s_recv.size_known ? s_recv.declared_size : s_recv.received + chunk_size);
            bool received = receive_data_phase(s_recv.sink, chunk_size, total, s_recv.received, progressCb);
            if (received) s_recv.received += chunk_size;

            if (!received) {
                send_response(PTP_RC_GENERAL_ERROR, cmd.transaction_id, nullptr, 0);
                finish_recv(false, state, progressCb);
                break;
            }
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_ANDROID_TRUNCATE_OBJECT: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            uint64_t size = read_param_u64(params, params_len, 1);
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            // Purely informational here - every content piece a .nsp/.xci
            // actually installs is already individually sized from its own
            // PFS0/HFS0 entry, so nothing downstream depends on the
            // container's own overall size. Kept only for
            // GetObjectPropValue/GetObjectPropList to report faithfully.
            s_recv.truncated_size = size;
            send_response(PTP_RC_OK, cmd.transaction_id, nullptr, 0);
            break;
        }
        case PTP_OP_ANDROID_END_EDIT_OBJECT: {
            uint32_t object_id = read_param_u32(params, params_len, 0);
            if (!s_recv.active || object_id != s_recv.handle) {
                send_response(PTP_RC_INVALID_OBJECT_HANDLE, cmd.transaction_id, nullptr, 0);
                break;
            }
            // Same install-then-answer ordering as plain SendObject's - see
            // the comment there.
            bool installed = finish_recv(true, state, progressCb);
            send_response(installed ? PTP_RC_OK : PTP_RC_GENERAL_ERROR, cmd.transaction_id, nullptr, 0);
            break;
        }
        default:
            send_response(PTP_RC_OPERATION_NOT_SUPPORTED, cmd.transaction_id, nullptr, 0);
            break;
    }
}

} // namespace

bool mtp_start(std::string workingRoot, install::InstallStorageTarget target, std::string& error) {
    s_session_open = false;
    s_next_object_handle = 2;
    s_recv = PendingReceive{};
    s_working_root = std::move(workingRoot);
    s_target = target;
    char err_buf[200] = {0};
    if (!mtp_usb_init(err_buf, sizeof(err_buf))) {
        error = err_buf;
        return false;
    }
    s_running = true;
    log_msg("[mtp] responder started\n");
    return true;
}

void mtp_stop() {
    mtp_usb_exit();
    s_running = false;
    s_session_open = false;
    log_msg("[mtp] responder stopped\n");
}

void mtp_step(MtpState& state, const MtpProgressCallback& progressCb) {
    if (!s_running) {
        state.status = MtpStatus::WaitingForUsb;
        return;
    }

    if (!mtp_usb_is_connected()) {
        state.status = MtpStatus::WaitingForUsb;
        s_session_open = false;
        return;
    }
    state.status = s_session_open ? MtpStatus::Idle : MtpStatus::WaitingForHost;

    // Deliberately doesn't answer the USB Still Image class's EP0 control
    // requests (Get Device Status, Cancel, Device Reset) - Atmosphère's own
    // MTP homebrew (troposphere/haze - a real, Windows-proven
    // implementation) never touches EP0 at all, only the bulk pipes below.

    // One read per container: a command's header and its (up to 5 u32)
    // parameters arrive together in a single transfer, so this reads a
    // whole packet and parses the header out of the front, keeping the rest
    // as the parameter bytes handle_command's individual cases read from.
    static uint8_t packet[512]; // one high-speed max packet; commands are at most 32 bytes
    int n = mtp_usb_read(packet, sizeof(packet), kPollTimeoutNs);
    if (n < (int)sizeof(PtpContainer)) return; // nothing arrived this poll, or a runt - next call tries again

    PtpContainer cmd;
    memcpy(&cmd, packet, sizeof(cmd));
    if (cmd.type != PTP_TYPE_COMMAND) return; // malformed - drop it

    handle_command(cmd, packet + sizeof(cmd), (size_t)n - sizeof(cmd), state, progressCb);
}

} // namespace pipensx::mtp
