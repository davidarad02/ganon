#include "logging.h"
#include "common.h"

int rc_demo(int a, int b, int *result_out) {
    int rc = 0;

    FAIL_IF(a == b,
            E__MAIN__RC_DEMO__SOME_ERROR);

    if (result_out != NULL) {
        *result_out = a + b;
    }

l_cleanup:
    return rc;
}

int loop_demo(int *arr, int count) {
    int rc = 0;
    int sum = 0;

    for (int i = 0; i < count; i++) {
        CONTINUE_IF(arr[i] < 0);
        BREAK_IF(sum > 100);
        sum += arr[i];
    }

l_cleanup:
    return sum;
}

int main() {
    int result = 0;

    rc_demo(1, 2, &result);
    LOG_INFO("rc_demo returned %d", result);

    int arr[] = {10, 20, -5, 30, 40};
    int sum = loop_demo(arr, 5);
    LOG_INFO("loop_demo sum: %d", sum);

    return 0;
}