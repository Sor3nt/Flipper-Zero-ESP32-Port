#pragma once

// Compat shim: upstream apps include <lib/flipper_application/plugins/composite_resolver.h>;
// in this port the real header lives one directory level up under flipper_application/.
// Mirrors the existing lib/flipper_application/flipper_application.h shim.
#include "../../../flipper_application/plugins/composite_resolver.h"
