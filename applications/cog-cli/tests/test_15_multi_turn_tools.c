/** @file test_15_multi_turn_tools.c
 * @brief Scenario15: typed model calls through durable async workspace tools.
 */
int cog_app_conformance_main(void);
#define main cog_app_conformance_main
#include "test_app_loop.c"
#undef main
int main(void)
{
    REQUIRE(tool_round(true, true) == 0);
    REQUIRE(tool_round(false, true) == 0);
    REQUIRE(tool_round(true, false) == 0);
    return 0;
}
