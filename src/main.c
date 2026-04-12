#include "logging.h"

int main() {
    LOG_INFO("Application started");
    LOG_DEBUG("Debug message");
    LOG_TRACE("Trace message");
    LOG_INFO("Application exiting");
    return 0;
}