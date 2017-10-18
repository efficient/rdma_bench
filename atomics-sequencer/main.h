#include <stdint.h>
#include <stdlib.h>

// This code can emulate either an array of 8B counters or DrTM-KV with
// 16B keys and 32B values.
//
// For array-of-counters, we want to measure peak atomics performance so
// BUF_SIZE should be small.
//
// DrTM emulation is a bit crude, but gives it a performance benefit by not
// using an RDMA write during UPDATE operations.
static constexpr bool kAppEmulateDrTM = true;

// Percentage of UPDATE operations in DrTM. Set to 100 for array of counters.
static constexpr size_t kAppUpdatePercentage = 10;

// Important: For ConnectX-3 at least, atomics throughput drops from 2.7 Mops
// to 1.8 Mops when the size of registered memory exceeds the CPU's cache size.
// This may be because of the higher latency to do read-modify-write into DRAM.
// For our experiments, 256K counters are enough.
static constexpr size_t kAppBufSize = (1024 * 1024 * 1024);

// USE_RANDOM = 1 means all clients choose the remote counter randomly.
// Otherwise all clients use the same remote counter.
static constexpr bool kAppUseRandom = true;

// Addresses picked for atomic access or READs are 0 modulo STRIDE_SIZE.
//
// STRIDE_SIZE = 64 emulates DrTM with 16-byte keys and 32-byte values. The
// remaining 16 bytes in a cacheline are occupied by version (4B),
// incarnation (4B), and state (8B) fields.
static constexpr size_t kAppStrideSize = 64;

static constexpr int kAppServerSHMKey = 24;
static constexpr size_t kAppMaxPostlist = 64;

static constexpr size_t kAppNumClients = 32;
static constexpr size_t kAppQPsPerClient = 2;

static constexpr size_t kAppUnsigBatch = 32;

struct thread_params_t {
  size_t id;  // Global ID of this client or server thread
};

// For every 100 DrTM key-value operations, there are (2 * kAppUpdatePercentage)
// atomic RDMA operations, and (100 - kAppUpdatePercentage) READ operations.
//
// To find whether a particular RDMA operation should be atomic, we generate a
// random number between 0 and (100 + kAppUpdatePercentage - 1) and check if it
// is smaller than (2 * kAppUpdatePercentage).
inline int drtm_use_atomic(uint64_t rand) {
  // Exammples:
  // kAppUpdatePercentage = 0	=> rand % 100 < 0		=> False
  // kAppUpdatePercentage = 5	=> rand % 105 < 10		=> ??
  // kAppUpdatePercentage = 100	=> rand % 200 < 200		=> True
  if (rand % (100 + kAppUpdatePercentage) < 2 * kAppUpdatePercentage) {
    return 1;
  } else {
    return 0;
  }
}
