#include "version.h"
#include <stdlib.h>
#include <string.h>

struct Version {
    const char* git_hash;
    const char* git_branch;
    const char* git_branch_num;
    const char* build_date;
    const char* version;
    const char* custom_name;
    const char* firmware_origin;
    const char* git_origin;
    uint8_t target;
    bool dirty_flag;
};

static Version firmware_version = {
    .git_hash = "esp32-dev",
    .git_branch = "esp32-port",
    .git_branch_num = "0",
    .build_date = __DATE__,
    .version = "1.4.3",
    .custom_name = NULL,
    .firmware_origin = "ESP32 Port",
    .git_origin = "local",
    .target = 32,
    .dirty_flag = true,
};

const Version* version_get(void) {
    return &firmware_version;
}

const char* version_get_githash(const Version* v) {
    if(!v) v = version_get();
    return v->git_hash;
}

const char* version_get_gitbranch(const Version* v) {
    if(!v) v = version_get();
    return v->git_branch;
}

const char* version_get_gitbranchnum(const Version* v) {
    if(!v) v = version_get();
    return v->git_branch_num;
}

const char* version_get_builddate(const Version* v) {
    if(!v) v = version_get();
    return v->build_date;
}

const char* version_get_version(const Version* v) {
    if(!v) v = version_get();
    return v->version;
}

const char* version_get_custom_name(const Version* v) {
    if(!v) v = version_get();
    return v->custom_name;
}

void version_set_custom_name(Version* v, const char* name) {
    if(!v) return;
    v->custom_name = name;
}

uint8_t version_get_target(const Version* v) {
    if(!v) v = version_get();
    return v->target;
}

bool version_get_dirty_flag(const Version* v) {
    if(!v) v = version_get();
    return v->dirty_flag;
}

const char* version_get_firmware_origin(const Version* v) {
    if(!v) v = version_get();
    return v->firmware_origin;
}

const char* version_get_git_origin(const Version* v) {
    if(!v) v = version_get();
    return v->git_origin;
}
