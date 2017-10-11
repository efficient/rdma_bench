#ifndef HRD_H
#define HRD_H

#include <assert.h>
#include <errno.h>
#include <numaif.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <libmemcached/memcached.h>
#include <malloc.h>
#include <time.h>
#include "hrd_sizes.h"

#define HRD_SQ_DEPTH 256  /* Depth of all SEND queues */
#define HRD_RQ_DEPTH 2048 /* Depth of all RECV queues */

#define HRD_DEFAULT_PSN 3185 /* PSN for all queues */
#define HRD_DEFAULT_QKEY 0x11111111
#define HRD_MAX_LID 256

#define HRD_QP_NAME_SIZE 200 /* Size (in bytes) of a queue pair name */
#define HRD_RESERVED_NAME_PREFIX "__HRD_RESERVED_NAME_PREFIX"

#define HRD_CONNECT_IB_ATOMICS 0

/*
 * Small max_inline_data reduces the QP's max WQE size, which reduces the
 * DMA size in doorbell method of WQE fetch.
 */
#define HRD_MAX_INLINE 60

/* Useful when `x = (x + 1) % N` is done in a loop */
#define HRD_MOD_ADD(x, N) \
  do {                    \
    x = x + 1;            \
    if (x == N) {         \
      x = 0;              \
    }                     \
  } while (0)

#define HRD_MIN(a, b) (a < b ? a : b)
#define HRD_MAX(a, b) (a > b ? a : b)

/* Compare, print, and exit */
#define CPE(val, msg, err_code)                \
  if (val) {                                   \
    fprintf(stderr, msg);                      \
    fprintf(stderr, " Error %d \n", err_code); \
    exit(err_code);                            \
  }

/* Compile time assert. !!(@condition) converts @condition into a 0/1 bool. */
#define ct_assert(condition) ((void)sizeof(char[-1 + 2 * !!(condition)]))

/* Ensure that x is between a and b, inclusive */
#define range_assert(x, a, b) (assert(x >= a && x <= b))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define forceinline inline __attribute__((always_inline))
#define _unused(x) ((void)(x)) /* Make production build happy */

/* Is pointer x aligned to A-byte alignment? */
#define is_aligned(x, A) (((uint64_t)x) % A == 0)

/* Registry info about a QP */
struct hrd_qp_attr_t {
  char name[HRD_QP_NAME_SIZE];

  /* Info about the RDMA buffer associated with this QP */
  uintptr_t buf_addr;
  uint32_t buf_size;
  uint32_t rkey;

  uint16_t lid;
  uint32_t qpn;
};

struct hrd_ctrl_blk_t {
  size_t local_hid; /* Local ID on the machine this process runs on */

  /* Info about the device/port to use for this control block */
  size_t port_index;   /* User-supplied. 0-based across all devices */
  size_t numa_node_id; /* NUMA node id */

  /// InfiniBand info resolved from \p phy_port, must be filled by constructor.
  struct {
    int device_id;               ///< Device index in list of verbs devices
    struct ibv_context* ib_ctx;  ///< The verbs device context
    uint8_t dev_port_id;         ///< 1-based port ID in device. 0 is invalid.
    uint16_t port_lid;           ///< LID of phy_port. 0 is invalid.
  } resolve;

  struct ibv_pd* pd; /* A protection domain for this control block */

  /* Connected QPs */
  bool use_uc;
  size_t num_conn_qps;
  struct ibv_qp** conn_qp;
  struct ibv_cq** conn_cq;
  volatile uint8_t* conn_buf; /* A buffer for RDMA over RC/UC QPs */
  size_t conn_buf_size;
  int conn_buf_shm_key;
  struct ibv_mr* conn_buf_mr;

  /* Datagram QPs */
  size_t num_dgram_qps;
  struct ibv_qp** dgram_qp;
  struct ibv_cq **dgram_send_cq, **dgram_recv_cq;
  volatile uint8_t* dgram_buf; /* A buffer for RECVs on dgram QPs */
  size_t dgram_buf_size;
  int dgram_buf_shm_key;
  struct ibv_mr* dgram_buf_mr;

  uint8_t pad[64];
};

struct hrd_conn_config_t {
  size_t num_qps;
  bool use_uc;
  volatile uint8_t* prealloc_buf;
  size_t buf_size;
  int buf_shm_key;
};

struct hrd_dgram_config_t {
  size_t num_qps;
  volatile uint8_t* prealloc_buf;
  size_t buf_size;
  int buf_shm_key;
};

/* Major initialzation functions */
hrd_ctrl_blk_t* hrd_ctrl_blk_init(size_t local_hid, size_t port_index,
                                  size_t numa_node_id,
                                  hrd_conn_config_t* conn_config,
                                  hrd_dgram_config_t* dgram_config);

int hrd_ctrl_blk_destroy(hrd_ctrl_blk_t* cb);

/* Debug */
void hrd_ibv_devinfo(void);

void hrd_resolve_port_index(hrd_ctrl_blk_t* cb, size_t port_index);
void hrd_create_conn_qps(hrd_ctrl_blk_t* cb);
void hrd_create_dgram_qps(hrd_ctrl_blk_t* cb);

void hrd_connect_qp(hrd_ctrl_blk_t* cb, size_t conn_qp_idx,
                    hrd_qp_attr_t* remote_qp_attr);

/* Post 1 RECV for this queue pair for this buffer. Low performance. */
void hrd_post_dgram_recv(struct ibv_qp* qp, void* buf_addr, size_t len,
                         uint32_t lkey);

/* Fill @wc with @num_comps comps from this @cq. Exit on error. */
static inline void hrd_poll_cq(struct ibv_cq* cq, int num_comps,
                               struct ibv_wc* wc) {
  int comps = 0;
  while (comps < static_cast<int>(num_comps)) {
    int new_comps = ibv_poll_cq(cq, num_comps - comps, &wc[comps]);
    if (new_comps != 0) {
      /* Ideally, we should check from comps -> new_comps - 1 */
      if (wc[comps].status != 0) {
        fprintf(stderr, "Bad wc status %d\n", wc[comps].status);
        exit(0);
      }

      comps += new_comps;
    }
  }
}

/* Fill @wc with @num_comps comps from this @cq. Return -1 on error, else 0. */
static inline int hrd_poll_cq_ret(struct ibv_cq* cq, int num_comps,
                                  struct ibv_wc* wc) {
  int comps = 0;

  while (comps < num_comps) {
    int new_comps = ibv_poll_cq(cq, num_comps - comps, &wc[comps]);
    if (new_comps != 0) {
      /* Ideally, we should check from comps -> new_comps - 1 */
      if (wc[comps].status != 0) {
        fprintf(stderr, "Bad wc status %d\n", wc[comps].status);
        return -1; /* Return an error so the caller can clean up */
      }

      comps += new_comps;
    }
  }

  return 0; /* Success */
}

/* Registry functions */
void hrd_publish(const char* key, void* value, size_t len);
int hrd_get_published(const char* key, void** value);

/* Publish the nth connected queue pair from this cb with this name */
void hrd_publish_conn_qp(hrd_ctrl_blk_t* cb, size_t n, const char* qp_name);

/* Publish the nth datagram queue pair from this cb with this name */
void hrd_publish_dgram_qp(hrd_ctrl_blk_t* cb, size_t n, const char* qp_name);

struct hrd_qp_attr_t* hrd_get_published_qp(const char* qp_name);

void hrd_publish_ready(const char* qp_name);
void hrd_wait_till_ready(const char* qp_name);

void hrd_close_memcached();

/* Utility functions */
static inline uint32_t hrd_fastrand(uint64_t* seed) {
  *seed = *seed * 1103515245 + 12345;
  return static_cast<uint32_t>((*seed) >> 32);
}

static inline size_t hrd_get_cycles() {
  uint64_t rax;
  uint64_t rdx;
  asm volatile("rdtsc" : "=a"(rax), "=d"(rdx));
  return static_cast<size_t>((rdx << 32) | rax);
}

static inline int hrd_is_power_of_2(uint64_t n) { return n && !(n & (n - 1)); }

uint8_t* hrd_malloc_socket(int shm_key, size_t size, size_t socket_id);
int hrd_free(int shm_key, void* shm_buf);
void hrd_red_printf(const char* format, ...);
void hrd_get_formatted_time(char* timebuf);
void hrd_nano_sleep(size_t ns);
char* hrd_getenv(const char* name);

#endif /* HRD_H */
