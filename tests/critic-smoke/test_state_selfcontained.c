#include "cog/state.h"

int main(void)
{
    static const cog_state_t allowed[] = {COG_TRANSITION_SELF, COG_TRANSITION_END};
    static const cog_dispatch_entry_t table[] = {
        COG_DISPATCH_ENTRY(0U, 1U, NULL, allowed),
    };
    (void)table;
    return 0;
}
