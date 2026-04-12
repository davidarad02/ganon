#include "logging.h"

int main() {
    LOG_INFO("Application started");
    LOG_DEBUG("Debug message");
    LOG_TRACE("Trace message");
    LOG_INFO("Application exiting");

    LOG_INFO("User %d logged in", 42);
    LOG_DEBUG("Value is %d, pointer is %p", 100, (void*)0x1234);
    LOG_TRACE("Processing item %s", "sword");

    return 0;
}