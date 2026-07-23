#include "es_ticket.h"

static Service g_es_srv;

Result es_initialize(void) {
    return smGetService(&g_es_srv, "es");
}

void es_exit(void) {
    serviceClose(&g_es_srv);
}

Result es_import_ticket(const void *tik, size_t tik_size, const void *cert, size_t cert_size) {
    return serviceDispatch(&g_es_srv, 1,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { tik, tik_size },
            { cert, cert_size },
        },
    );
}
