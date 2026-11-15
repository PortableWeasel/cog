/** @file core_service_store_double.c
 * @brief Link isolation for scheduler tests while persistence migrates separately.
 * No test supplies a store; accidental use fails loudly. Never production code.
 */
#include <cog/session_store.h>
#include <assert.h>
bool cog_store_auto_save(const cog_store_t *store)
{
    (void)store;
    assert(0);
    return false;
}
cog_result_t cog_store_save(cog_store_t *store, const cog_session_t *session)
{
    (void)store;
    (void)session;
    assert(0);
    return COG_ERROR_UNSUPPORTED;
}
