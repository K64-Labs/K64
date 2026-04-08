#include "k64_reload.h"
#include "k64_system.h"

typedef struct {
    k64_reload_mode_t mode;
    bool pending;
} k64_reload_request_t;

static k64_reload_request_t reload_request = { K64_RELOAD_NONE, false };

bool k64_reload_request(k64_reload_mode_t mode) {
    k64_service_result_t result;

    reload_request.mode = mode;
    reload_request.pending = true;
    result = k64_system_start_service_by_name("reload");
    return result == K64_SERVICE_OK || result == K64_SERVICE_ERR_ALREADY_RUNNING;
}

k64_reload_mode_t k64_reload_take_request(void) {
    k64_reload_mode_t mode = reload_request.mode;

    reload_request.mode = K64_RELOAD_NONE;
    reload_request.pending = false;
    return mode;
}
