/* Negative fixture: Release would silently remove the assertion workload. */
#include <assert.h>
#include <stdio.h>
int main(void)
{
    int executed = 0;
    assert(++executed == 1);
    printf("executed=%d\n", executed);
    return 0;
}
