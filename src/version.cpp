// Own translation unit on purpose: keeps the version (regenerated on every
// commit) out of main.cpp, so a version bump recompiles only this tiny file
// instead of the heavy main.cpp TU (which pulls in httplib/prometheus headers).
#include "l2-proxy-version.h"

const char *g_l2_proxy_version = VERSION;
