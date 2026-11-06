#include "sqlite3.h"
#include "vexdb_sqlite.h"

#include <stdio.h>
#include <stdlib.h>

void vexdb_shell_init(void) {
    const int rc = sqlite3_auto_extension((void (*)(void))sqlite3_vexdblite_init);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vexdb: cannot register VexDB-Lite SQLite extension: %s\n",
                sqlite3_errstr(rc));
        exit(1);
    }
}
