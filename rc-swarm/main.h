#ifndef MAIN_H
#define MAIN_H

#include <gflags/gflags.h>
#include <stdlib.h>
#include <array>
#include "libhrd_cpp/hrd.h"
#include "sweep.h"

// If 1, all RDMA are signaled and UNSIG_BATCH is ignored
static constexpr bool kAppAllsig = true;

static constexpr size_t kAppBufSize = MB(2);
static constexpr size_t kAppBufSize_ = (kAppBufSize - 1);

// SHM keys used by workers
static constexpr int kAppWorkerBaseSHMKey = 24;

// Number of outstanding requests kept by a worker thread  across all QPs.
// This is only used for READs where we can detect completion by polling on
// a READ's destination buffer.
//
// For WRITEs, this is hard to do unless we make every send() signaled. So,
// the number of per-thread outstanding operations per thread with WRITEs is
// O(NUM_CLIENTS * UNSIG_BATCH).

static constexpr size_t kAppNumMachines = 8;
static constexpr size_t kAppNumVirtualMachines =
    kAppNumMachines * kAppVMPerMachine;

static constexpr size_t kAppNumThreads = kAppNumWorkers / kAppNumMachines;
static constexpr size_t kAppNumQPsPerThread = kAppNumVirtualMachines;
static constexpr size_t kAppUnsigBatch_ = (kAppUnsigBatch - 1);

// Upper bounds to avoid dynamic alloc
static constexpr size_t kAppMaxPorts = 2;
static constexpr size_t kAppMaxMachines = 256;  // Max machines in the swarm

// Checks
static_assert(kAppNumWorkers % kAppNumMachines == 0, "");
static_assert(kAppBufSize >= MB(2), "");    // Large buffer, more parallelism
static_assert(kAppNumMachines >= 2, "");  // At least 2 machines
static_assert(kAppNumWorkers % kAppNumMachines == 0, "");  // kAppNumThreads
static_assert(kHrdSQDepth == 128, "");                 // Reduce cache pressure
static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "");  // Queue capacity check

struct thread_params_t {
  size_t wrkr_gid;
  size_t wrkr_lid;
  double* tput_arr;
};

void run_worker(thread_params_t* params);

DEFINE_uint64(numa_node, 0, "NUMA node");
DEFINE_uint64(use_uc, 0, "Use UC?");
DEFINE_uint64(base_port_index, 0, "Base port index");
DEFINE_uint64(num_ports, 0, "Number of ports");
DEFINE_uint64(machine_id, 0, "ID of this machine");
DEFINE_uint64(do_read, 0, "Use RDMA READs?");

static bool validate_do_read(const char*, uint64_t do_read) {
  // Size checks based on opcode. This is not UD, so no 0-byte read/write.
  if (do_read == 0) {  // For WRITEs
    return kAppSize >= 1 && kAppSize <= kHrdMaxInline;
  } else {
    return kAppSize >= 1 && kAppSize <= kAppBufSize;
  }
}

DEFINE_validator(do_read, &validate_do_read);

#endif  // MAIN_H
