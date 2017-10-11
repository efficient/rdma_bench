#include <stdint.h>
#include <stdlib.h>

/*
 * This code can emulate either an array of 8B counters or DrTM-KV with
 * 16B keys and 32B values.
 *
 * For array-of-counters, we want to measure peak atomics performance so
 * BUF_SIZE should be small.
 *
 * DrTM emulation is a bit crude, but gives it a performance benefit by not
 * using an RDMA write during UPDATE operations.
 */
#define EMULATE_DRTM 1

/* Percentage of UPDATE operations in DrTM. Set to 100 for array of counters. */
#define UPDATE_PERCENTAGE 10

/*
 * Important: For ConnectX-3 at least, atomics throughput drops from 2.7 Mops
 * to 1.8 Mops when the size of registered memory exceeds the CPU's cache size.
 * This may be because of the higher latency to do read-modify-write into DRAM.
 * For our experiments, 256K counters are enough.
 */
#define BUF_SIZE (1024 * 1024 * 1024)

/*
 * USE_RANDOM = 1 means all clients choose the remote counter randomly.
 * Otherwise all clients use the same remote counter.
 */
#define USE_RANDOM 1

/*
 * Addresses picked for atomic access or READs are 0 modulo STRIDE_SIZE.
 *
 * STRIDE_SIZE = 64 emulates DrTM with 16-byte keys and 32-byte values. The
 * remaining 16 bytes in a cacheline are occupied by version (4B),
 * incarnation (4B), and state (8B) fields.
 */
#define STRIDE_SIZE 64

#define SERVER_SHM_KEY 24
#define MAX_POSTLIST 64

#define NUM_CLIENTS 32 /* Number of client threads (not client QPs) */
#define QPS_PER_CLIENT 2

#define UNSIG_BATCH 32
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

struct thread_params_t {
  size_t id; /* Global ID of this client or server thread */
};

/*
 * For every 100 DrTM key-value operations, there are (2 * UPDATE_PERCENTAGE)
 * atomic RDMA operations, and (100 - UPDATE_PERCENTAGE) READ operations.
 *
 * To find whether a particular RDMA operation should be atomic, we generate a
 * random number between 0 and (100 + UPDATE_PERCENTAGE - 1) and check if it
 * is smaller than (2 * UPDATE_PERCENTAGE).
 */
inline int drtm_use_atomic(uint64_t rand) {
  /*
   * Exammples:
   * UPDATE_PERCENTAGE = 0	=> rand % 100 < 0		=> False
   * UPDATE_PERCENTAGE = 5	=> rand % 105 < 10		=> ??
   * UPDATE_PERCENTAGE = 100	=> rand % 200 < 200		=> True
   */
  if (rand % (100 + UPDATE_PERCENTAGE) < 2 * UPDATE_PERCENTAGE) {
    return 1;
  } else {
    return 0;
  }
}
