#include <string.h>
#include <stdlib.h>

#include "common.h"
#include "logging.h"
#include "skin.h"

#define SKIN_MAX_REGISTERED 8

static const skin_ops_t *g_skins[SKIN_MAX_REGISTERED];
static int g_skin_count = 0;
static uint32_t g_default_skin_id = SKIN_ID__TCP_MONOCYPHER;

err_t SKIN__register(const skin_ops_t *ops) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(ops);

    if (g_skin_count >= SKIN_MAX_REGISTERED) {
        LOG_ERROR("Skin registry full (max %d)", SKIN_MAX_REGISTERED);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    g_skins[g_skin_count++] = ops;
    LOG_INFO("Registered skin '%s' (id=%u)", ops->name, ops->skin_id);

l_cleanup:
    return rc;
}

const skin_ops_t *SKIN__by_id(uint32_t skin_id) {
    for (int i = 0; i < g_skin_count; i++) {
        if (g_skins[i]->skin_id == skin_id) {
            return g_skins[i];
        }
    }
    return NULL;
}

const skin_ops_t *SKIN__by_name(const char *name) {
    if (NULL == name) {
        return NULL;
    }
    for (int i = 0; i < g_skin_count; i++) {
        if (0 == strcmp(g_skins[i]->name, name)) {
            return g_skins[i];
        }
    }
    return NULL;
}

const skin_ops_t *SKIN__default(void) {
    return SKIN__by_id(g_default_skin_id);
}

void SKIN__set_default(uint32_t skin_id) {
    g_default_skin_id = skin_id;
}
