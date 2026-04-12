#include "logging.h"
#include "common.h"

int rc_demo(int a, int b, int *result_out) {
    int rc = 0;

    FAIL_IF(a == b, E__MAIN__RC_DEMO__SOME_ERROR);

    if (result_out != NULL) {
        *result_out = a + b;
    }

l_cleanup:
    return rc;
}

int main() {
    int result = 0;

    rc_demo(1, 2, &result);
    LOG_INFO("rc_demo returned %d", result);

    return 0;
}