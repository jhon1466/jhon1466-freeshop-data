#pragma once
#include <switch.h>

// Matches Nintendo's ns::ApplicationRecordContentMetaKey layout: the content
// meta key plus the storage id it lives on.
typedef struct {
    NcmContentMetaKey meta_record;
    uint8_t storage_id;
} NsContentStorageRecord;

// Not exposed by libnx - public, documented enum values (switchbrew.org).
typedef enum {
    NsRecordType_Installed       = 0x3,
    NsRecordType_GamecardMissing = 0x5,
    NsRecordType_Archived        = 0xB,
} NsRecordType;

// libnx exposes nsGetApplicationManagerInterface() but not the
// PushApplicationRecord command on top of it (command 16 on
// IApplicationManagerInterface - public, documented on switchbrew.org).
// Without this, an installed title works but doesn't show up on the
// hbmenu/home-menu application list.
Result ns_record_initialize(void);
void ns_record_exit(void);

Result ns_push_application_record(uint64_t application_id, uint8_t last_modified_event,
                                   const NsContentStorageRecord *records, uint32_t count);
