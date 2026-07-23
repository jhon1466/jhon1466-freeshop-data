#pragma once
#include <switch.h>

// libnx doesn't wrap the "es" (eTicket) service - only ImportTicket
// (command 1, a well-documented public IPC command on switchbrew.org) is
// needed here, so it's hand-rolled directly on top of Service/serviceDispatch
// instead of pulling in a whole ES binding.

Result es_initialize(void);
void es_exit(void);

// Imports a common ticket + its certificate chain, granting rights to the
// title(s) it covers. Both buffers are read directly from the source NSP's
// PFS0 - no key material of any kind is needed here.
Result es_import_ticket(const void *tik, size_t tik_size, const void *cert, size_t cert_size);
