#include "ns_record.h"

static Service g_ns_am_srv;

Result ns_record_initialize(void) {
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return rc;

    rc = nsGetApplicationManagerInterface(&g_ns_am_srv);
    if (R_FAILED(rc)) nsExit();
    return rc;
}

void ns_record_exit(void) {
    serviceClose(&g_ns_am_srv);
    nsExit();
}

Result ns_push_application_record(uint64_t application_id, uint8_t last_modified_event,
                                   const NsContentStorageRecord *records, uint32_t count) {
    struct {
        uint8_t last_modified_event;
        uint64_t application_id;
    } in = { last_modified_event, application_id };

    return serviceDispatchIn(&g_ns_am_srv, 16, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { records, count * sizeof(*records) } },
    );
}
