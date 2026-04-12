#include "common.h"
#include "logging.h"

err_t rc_demo(int a, int b, int *result_out) {
    err_t rc = E__SUCCESS;

    FAIL_IF(a == b,
            E__MAIN__RC_DEMO__SOME_ERROR);

    if (NULL != result_out) {
        *result_out = a + b;
    }

l_cleanup:
    return rc;
}

err_t loop_demo(const int *arr, int count, int *sum_out) {
    err_t rc = E__SUCCESS;
    int sum = 0;

    for (int i = 0; i < count; i++) {
        CONTINUE_IF(0 > arr[i]);
        BREAK_IF(100 < sum);
        sum += arr[i];
    }

    if (NULL != sum_out) {
        *sum_out = sum;
    }

l_cleanup:
    return rc;
}

err_t main(void) {
    err_t rc = E__SUCCESS;
    int result = 0;

    FAIL_IF(E__SUCCESS != rc_demo(1, 2, &result),
            E__MAIN__FAILURE);

    LOG_INFO("rc_demo returned %d", result);

    const int arr[] = {10, 20, -5, 30, 40};
    int sum = 0;

    FAIL_IF(E__SUCCESS != loop_demo(arr, (int)(sizeof(arr) / sizeof(arr[0])), &sum),
            E__MAIN__FAILURE);

    LOG_INFO("loop_demo sum: %d", sum);

l_cleanup:
    return rc;
}