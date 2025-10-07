/*
 * lb_core_advanced.c
 *
 * Improved load balancer core module with:
 * - Strong typing via explicit typedefs and enums
 * - Defensive error handling with status codes
 * - Modular backend selection strategy interface
 * - Thread-pool stub and lifecycle wiring
 * - Advanced structured logging macros (levels, categories, errno context)
 * - Extensive comments comparing improvements over legacy lb_core.c
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <stdatomic.h>

#include "core/loadbalancer.h" /* Reuse existing public types where available */

/* ========================= Advanced logging ========================= */

typedef enum {
    LB_LOG_DEBUG = 0,
    LB_LOG_INFO  = 1,
    LB_LOG_WARN  = 2,
    LB_LOG_ERROR = 3
} lb_log_level_t;

#ifndef LB_LOG_DEFAULT_LEVEL
#define LB_LOG_DEFAULT_LEVEL LB_LOG_INFO
#endif

static atomic_int g_log_level = ATOMIC_VAR_INIT(LB_LOG_DEFAULT_LEVEL);

static inline const char* lb_log_level_str(lb_log_level_t lvl) {
    switch (lvl) {
        case LB_LOG_DEBUG: return "DEBUG";
        case LB_LOG_INFO:  return "INFO";
        case LB_LOG_WARN:  return "WARN";
        case LB_LOG_ERROR: return "ERROR";
        default:           return "UNK";
    }
}

static inline void lb_logv(lb_log_level_t lvl, const char* cat,
                           const char* file, int line, const char* fn,
                           const char* fmt, va_list ap) {
    if (lvl < atomic_load(&g_log_level)) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    char tbuf[32];
    struct tm tmv; time_t sec = ts.tv_sec; localtime_r(&sec, &tmv);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "%s.%03ld [%s] (%s) %s:%d %s: ",
            tbuf, ts.tv_nsec/1000000, lb_log_level_str(lvl), cat, file, line, fn);
    vfprintf(stderr, fmt, ap);
    if (errno) fprintf(stderr, " (errno=%d: %s)", errno, strerror(errno));
    fputc('\n', stderr);
}

static inline void lb_log(lb_log_level_t lvl, const char* cat,
                          const char* file, int line, const char* fn,
                          const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    lb_logv(lvl, cat, file, line, fn, fmt, ap);
    va_end(ap);
}

#define LB_LOGD(cat, fmt, ...) lb_log(LB_LOG_DEBUG, cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LB_LOGI(cat, fmt, ...) lb_log(LB_LOG_INFO,  cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LB_LOGW(cat, fmt, ...) lb_log(LB_LOG_WARN,  cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LB_LOGE(cat, fmt, ...) lb_log(LB_LOG_ERROR, cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* ========================= Strong typing ========================= */

typedef enum {
    LB_OK = 0,
    LB_ERR_NOMEM = -1,
    LB_ERR_SYS   = -2,
    LB_ERR_INVAL = -3,
    LB_ERR_STATE = -4,
    LB_ERR_LIMIT = -5,
    LB_ERR_EMPTY = -6
} lb_status_t;

static inline const char* lb_status_str(lb_status_t s) {
    switch (s) {
        case LB_OK:        return "OK";
        case LB_ERR_NOMEM: return "NOMEM";
        case LB_ERR_SYS:   return "SYS";
        case LB_ERR_INVAL: return "INVAL";
        case LB_ERR_STATE: return "STATE";
        case LB_ERR_LIMIT: return "LIMIT";
        case LB_ERR_EMPTY: return "EMPTY";
        default:           return "?";
    }
}

/* Forward declarations of private structures expected by loadbalancer.h */
#ifndef MAX_BACKENDS
#define MAX_BACKENDS 256
#endif
#ifndef MAX_CONNECTIONS
#define MAX_CONNECTIONS 65535
#endif

/* ========================= Strategy interface ========================= */

typedef struct lb_backend backend_t; /* forward compatible with project */
typedef struct lb_core lb_core_t;

/* Pluggable backend selection strategy */
typedef struct lb_strategy {
    const char* name;
    lb_status_t (*init)(lb_core_t*);
    backend_t*  (*select)(lb_core_t*, const struct sockaddr_in* client);
    void        (*teardown)(lb_core_t*);
} lb_strategy_t;

/* ========================= Thread pool stub ========================= */

typedef struct lb_thread_pool {
    pthread_t* threads;
    uint32_t   nthreads;
    atomic_bool running;
} lb_thread_pool_t;

static lb_status_t lb_thread_pool_start(lb_thread_pool_t* tp, uint32_t nthreads) {
    if (!tp || nthreads == 0) return LB_ERR_INVAL;
    tp->threads = (pthread_t*)calloc(nthreads, sizeof(pthread_t));
    if (!tp->threads) return LB_ERR_NOMEM;
    tp->nthreads = nthreads;
    atomic_store(&tp->running, true);
    /* Stub: Threads would run an event loop or work-queue consumption */
    /* For now, do not actually spawn threads to keep CI safe. */
    return LB_OK;
}

static void lb_thread_pool_stop(lb_thread_pool_t* tp) {
    if (!tp) return;
    atomic_store(&tp->running, false);
    /* Stub: join threads if they were started */
    free(tp->threads); tp->threads = NULL; tp->nthreads = 0;
}

/* ========================= Core object ========================= */

typedef struct lb_core {
    uint16_t        port;
    atomic_bool     running;
    int             epfd;
    uint32_t        backend_count;
    backend_t*      backends[MAX_BACKENDS];
    atomic_uint     rr_idx;
    lb_algorithm_t  algorithm; /* Reuse existing enum from project */

    /* Config (mirrors existing but typed) */
    struct {
        uint32_t connect_timeout_ms;
        uint32_t read_timeout_ms;
        uint32_t write_timeout_ms;
        uint32_t keepalive_timeout_ms;
        uint32_t health_check_interval_ms;
        uint32_t max_connections;
        bool     tcp_nodelay;
        bool     so_reuseport;
        bool     defer_accept;
    } cfg;

    /* Memory pool (optional mapping) */
    void*          mem;
    size_t         mem_size;

    /* Concurrency */
    pthread_spinlock_t backends_lock;

    /* Strategy plugin */
    lb_strategy_t strategy;

    /* Thread pool stub */
    lb_thread_pool_t tpool;
} lb_core_t;

/* ========================= Utility helpers ========================= */

static inline int lb_set_nb_tcp_opts(int fd, bool reuseport, bool nodelay) {
    int rv = 0; int val = 1;
    rv |= setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
#ifdef SO_REUSEPORT
    if (reuseport) rv |= setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
#endif
#ifdef TCP_NODELAY
    if (nodelay) rv |= setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
#endif
    return rv;
}

static inline int lb_bind_listen(uint16_t port, bool reuseport, bool nodelay) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (lb_set_nb_tcp_opts(fd, reuseport, nodelay) != 0) { close(fd); return -1; }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = { htonl(INADDR_ANY) } };
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    if (listen(fd, SOMAXCONN) != 0) { close(fd); return -1; }
    return fd;
}

/* ========================= Strategies (modular) ========================= */

/* Least-connections strategy as a default robust choice */
static backend_t* strat_leastconn_select(lb_core_t* lb, const struct sockaddr_in* client) {
    (void)client;
    backend_t* sel = NULL; uint32_t minc = UINT32_MAX;
    for (uint32_t i = 0; i < lb->backend_count; ++i) {
        backend_t* b = lb->backends[i];
        if (!b) continue;
        /* Assuming project defines: state, active_conns */
        if (atomic_load(&b->state) == BACKEND_UP) {
            uint32_t c = atomic_load(&b->active_conns);
            if (c < minc) { minc = c; sel = b; }
        }
    }
    return sel;
}

static lb_status_t strat_leastconn_init(lb_core_t* lb) { (void)lb; return LB_OK; }
static void        strat_leastconn_teardown(lb_core_t* lb) { (void)lb; }

static lb_strategy_t STRAT_LEASTCONN = {
    .name = "leastconn",
    .init = strat_leastconn_init,
    .select = strat_leastconn_select,
    .teardown = strat_leastconn_teardown
};

/* Round-robin strategy */
static backend_t* strat_rr_select(lb_core_t* lb, const struct sockaddr_in* client) {
    (void)client;
    if (lb->backend_count == 0) return NULL;
    for (uint32_t tries = 0; tries < lb->backend_count; ++tries) {
        uint32_t idx = atomic_fetch_add(&lb->rr_idx, 1) % lb->backend_count;
        backend_t* b = lb->backends[idx];
        if (b && atomic_load(&b->state) == BACKEND_UP) return b;
    }
    return NULL;
}

static lb_strategy_t STRAT_RR = {
    .name = "roundrobin",
    .init = strat_leastconn_init,
    .select = strat_rr_select,
    .teardown = strat_leastconn_teardown
};

/* Factory: choose strategy based on lb_algorithm_t */
static lb_strategy_t lb_strategy_from_algo(lb_algorithm_t a) {
    switch (a) {
        case LB_ALGO_LEASTCONN: return STRAT_LEASTCONN;
        case LB_ALGO_ROUNDROBIN: return STRAT_RR;
        default: /* Fallback to leastconn for safety */ return STRAT_LEASTCONN;
    }
}

/* ========================= Public-ish API (advanced impl) ========================= */

/* Create and initialize core object with safe defaults */
static lb_status_t lb_core_create(uint16_t port, lb_algorithm_t algo, lb_core_t** out) {
    if (!out) return LB_ERR_INVAL;
    *out = NULL;
    lb_core_t* lb = (lb_core_t*)calloc(1, sizeof *lb);
    if (!lb) return LB_ERR_NOMEM;

    lb->port = port;
    atomic_store(&lb->running, false);
    lb->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (lb->epfd < 0) { free(lb); return LB_ERR_SYS; }

    lb->cfg.connect_timeout_ms = 5000;
    lb->cfg.read_timeout_ms = 30000;
    lb->cfg.write_timeout_ms = 30000;
    lb->cfg.keepalive_timeout_ms = 60000;
    lb->cfg.health_check_interval_ms = 5000;
    lb->cfg.max_connections = MAX_CONNECTIONS;
    lb->cfg.tcp_nodelay = true;
    lb->cfg.so_reuseport = true;
    lb->cfg.defer_accept = true;

    pthread_spin_init(&lb->backends_lock, PTHREAD_PROCESS_PRIVATE);

    lb->mem_size = 64ul * 1024 * 1024; /* 64MB lighter than legacy 1GB */
    lb->mem = mmap(NULL, lb->mem_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (lb->mem == MAP_FAILED) {
        lb->mem = NULL; lb->mem_size = 0; /* continue without pool */
        LB_LOGW("core", "Memory pool disabled: mmap failed");
    }

    lb->algorithm = algo;
    lb->strategy = lb_strategy_from_algo(algo);
    lb_status_t s = (lb->strategy.init ? lb->strategy.init(lb) : LB_OK);
    if (s != LB_OK) {
        LB_LOGE("core", "Strategy init failed: %s", lb->strategy.name);
        close(lb->epfd); pthread_spin_destroy(&lb->backends_lock); free(lb); return s;
    }

    /* Thread pool (stub) */
    long cpu = sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t nthreads = (uint32_t)(cpu > 0 ? cpu : 1);
    s = lb_thread_pool_start(&lb->tpool, nthreads);
    if (s != LB_OK) {
        LB_LOGW("core", "Thread-pool not started: %s", lb_status_str(s));
        /* Not fatal for stub */
    }

    *out = lb;
    LB_LOGI("core", "lb_core created on port %u with strategy %s", (unsigned)port, lb->strategy.name);
    return LB_OK;
}

static void lb_core_destroy(lb_core_t* lb) {
    if (!lb) return;
    lb_thread_pool_stop(&lb->tpool);
    if (lb->strategy.teardown) lb->strategy.teardown(lb);
    if (lb->mem) munmap(lb->mem, lb->mem_size);
    if (lb->epfd >= 0) close(lb->epfd);
    pthread_spin_destroy(&lb->backends_lock);
    for (uint32_t i = 0; i < lb->backend_count; ++i) {
        if (lb->backends[i]) {
            pthread_spin_destroy(&lb->backends[i]->lock);
            free(lb->backends[i]);
        }
    }
    free(lb);
}

static lb_status_t lb_core_add_backend(lb_core_t* lb, const char* host, uint16_t port, uint32_t weight) {
    if (!lb || !host || !port) return LB_ERR_INVAL;
    if (lb->backend_count >= MAX_BACKENDS) return LB_ERR_LIMIT;
    backend_t* b = (backend_t*)calloc(1, sizeof *b);
    if (!b) return LB_ERR_NOMEM;
    strncpy(b->host, host, sizeof(b->host)-1);
    b->port = port;
    atomic_store(&b->weight, weight ? weight : 1);
    atomic_store(&b->state, BACKEND_DOWN);
    b->sockfd = -1;
    pthread_spin_init(&b->lock, PTHREAD_PROCESS_PRIVATE);

    pthread_spin_lock(&lb->backends_lock);
    lb->backends[lb->backend_count++] = b;
    pthread_spin_unlock(&lb->backends_lock);

    LB_LOGI("backend", "Added backend %s:%u weight=%u", b->host, (unsigned)b->port, (unsigned)atomic_load(&b->weight));
    return LB_OK;
}

static backend_t* lb_core_select_backend(lb_core_t* lb, const struct sockaddr_in* client) {
    if (!lb || lb->backend_count == 0) return NULL;
    return lb->strategy.select
