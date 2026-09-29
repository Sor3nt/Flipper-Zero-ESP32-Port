/* Reuse the matching hardware profile without duplicating its implementation. */
#include "wdgwars_map_profile.h"
#if WDG_MAP_PLUS_PROFILE
#include "../wardriver_plus_i2c/wardriver_config.c"
#else
#include "../wardriver/wardriver_config.c"
#endif
