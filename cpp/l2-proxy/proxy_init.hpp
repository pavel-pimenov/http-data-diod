#ifndef PROXY_INIT_HPP
#define PROXY_INIT_HPP

class AppContext;

// Proxy-mode runtime components: NATS client, rate limiters, duplicate
// detector and the per-IP/per-client dynamic-label metric collectors.
// Lives in its own TU so AppContext stays constructible in unit tests
// without linking the NATS client library (tests run in worker/l2-server
// modes, where the proxy components are never created).
void init_proxy_components(AppContext &app_ctx);

#endif // PROXY_INIT_HPP