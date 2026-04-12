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

int loop_demo(const int *arr, int count) {
    int sum = 0;

    for (int i = 0; i < count; i++) {
        CONTINUE_IF(arr[i] < 0);
        BREAK_IF(sum > 100);
        sum += arr[i];
    }

    return sum;
}

int main(void) {
    int result = 0;

    (void)rc_demo(1, 2, &result);
    LOG_INFO("rc_demo returned %d", result);

    const int arr[] = {10, 20, -5, 30, 40};
    int sum = loop_demo(arr, (int)(sizeof(arr) / sizeof(arr[0])));
    LOG_INFO("loop_demo sum: %d", sum);

    return 0;
}