#include "common.h"
#include "logging.h"
#include "args.h"

err_t rc_demo(int a, int b, int *result_out) {
    err_t rc = E__SUCCESS;

    FAIL_IF(b == a,
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

int main(int argc, char *argv[]) {
    err_t rc = E__SUCCESS;
    args_t args;

    rc = args_parse(&args, argc, argv);
    FAIL_IF(E__SUCCESS != rc, rc);

    LOG_DEBUG("Starting main()");
    LOG_INFO("Listen IP: %s", args.listen_addr.ip);
    LOG_INFO("Listen Port: %d", args.listen_addr.port);
    for (int i = 0; i < args.connect_count; i++) {
        LOG_INFO("Connect[%d]: %s:%d", i, args.connect_addrs[i].ip, args.connect_addrs[i].port);
    }

l_cleanup:
    return (int)rc;
}