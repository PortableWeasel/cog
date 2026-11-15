#include "cog/session_store.h"

int main(void)
{
    cog_store_config_t cfg = {0};
    cog_store_vtable_t vt = {0};

    (void)cfg;
    (void)vt;
    (void)&cog_store_file_backend;
    (void)&cog_store_null_backend;
    return 0;
}