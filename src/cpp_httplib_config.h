// Configuration overrides for cpp-httplib performance optimization
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 64
#endif

#ifndef CPPHTTPLIB_KEEPALIVE_MAX_COUNT
#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 200
#endif

#ifndef CPPHTTPLIB_RECV_BUFSIZ
#define CPPHTTPLIB_RECV_BUFSIZ size_t(1048576u) // 1MB buffer for receiving
#endif

#ifndef CPPHTTPLIB_SEND_BUFSIZ
#define CPPHTTPLIB_SEND_BUFSIZ size_t(1048576u) // 1MB buffer for sending
#endif

#ifndef CPPHTTPLIB_COMPRESSION_BUFSIZ
#define CPPHTTPLIB_COMPRESSION_BUFSIZ size_t(1048576u) // 1MB compression buffer
#endif

// Enable TCP_NODELAY for lower latency
#ifndef CPPHTTPLIB_TCP_NODELAY
#define CPPHTTPLIB_TCP_NODELAY true
#endif

// Increase timeout values for handling larger payloads
#ifndef CPPHTTPLIB_READ_TIMEOUT_SECOND
#define CPPHTTPLIB_READ_TIMEOUT_SECOND 300
#endif

#ifndef CPPHTTPLIB_WRITE_TIMEOUT_SECOND
#define CPPHTTPLIB_WRITE_TIMEOUT_SECOND 300
#endif
