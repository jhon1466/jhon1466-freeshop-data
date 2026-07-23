#include "xci_container.h"

#include <stdio.h>

int xci_open_secure_partition(const char *path, Hfs0 *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    Hfs0 root;
    int rc = hfs0_parse_at(fp, XCI_ROOT_HFS0_OFFSET, &root);
    if (rc != 0) {
        fclose(fp);
        return rc;
    }

    int secure_index = hfs0_find_by_name(&root, "secure");
    if (secure_index < 0) {
        fclose(fp);
        return -2;
    }

    uint64_t secure_offset = hfs0_entry_file_offset(&root, secure_index);
    rc = hfs0_parse_at(fp, secure_offset, out);

    fclose(fp);
    return rc;
}
