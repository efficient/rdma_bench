#ifndef MAIN_H
#define MAIN_H

#include <gflags/gflags.h>
#include <stdlib.h>
#include <array>
#include "libhrd_cpp/hrd.h"
#include "sweep.h"

// kAppWindowSize is the number of outstanding requests kept by a worker thread
// across all its QPs. This is only used for READs where we can detect
// completion by polling on a READ's destination buffer.
//
// For WRITEs, this is hard to do unless we make every send() signaled. So,
// the number of per-thread outstanding operations per thread with WRITEs is
// O(NUM_CLIENTS * UNSIG_BATCH).

static constexpr size_t kAppBufSize = MB(2);
static_assert(is_power_of_two(kAppBufSize), "");

// Round RDMA source and target offset address to a cacheline boundary. This
// can increase or decrease performance.
static constexpr bool kAppRoundOffset = true;

static constexpr size_t kAppMaxPorts = 2;        // Max ports for a thread
static constexpr size_t kAppMaxMachines = 256;   // Max machines in the swarm
static constexpr size_t kAppMaxThreads = 28;     // Max threads on a machine
static constexpr size_t kAppMaxWindow = 64;      // Max window size
static constexpr int kAppWorkerBaseSHMKey = 24;  // SHM keys used by workers

// Checks
static_assert(kHrdMaxInline == 16, "");  // For single-cacheline WQEs

static_assert(kAppBufSize >= MB(2), "");  // Large buffer, more parallelism
static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "");  // Queue capacity check

struct thread_params_t {
  size_t wrkr_gid;
  size_t wrkr_lid;
  double* tput_arr;
};

// Flags
DEFINE_uint64(machine_id, SIZE_MAX, "ID of this machine");
DEFINE_uint64(num_machines, SIZE_MAX, "Physical machines in the cluster");
DEFINE_uint64(num_threads, SIZE_MAX, "Threads per machine");
DEFINE_uint64(vms_per_machine, SIZE_MAX, "VMs per physical machine");
DEFINE_uint64(base_port_index, SIZE_MAX, "Base port index");
DEFINE_uint64(numa_node, SIZE_MAX, "NUMA node");
DEFINE_uint64(num_ports, SIZE_MAX, "Number of ports");
DEFINE_uint64(do_read, SIZE_MAX, "Use RDMA READs?");
DEFINE_uint64(size, SIZE_MAX, "RDMA size");
DEFINE_uint64(window_size, SIZE_MAX, "RDMA operation window size");

// File I/O helpers

// Record machine throughput
void record_sweep_params(FILE* fp) {
  fprintf(fp, "Machine %zu: sweep parameters: ", FLAGS_machine_id);
  fprintf(fp, "Threads per machine %zu, ", FLAGS_num_threads);
  fprintf(fp, "RDMA size %zu, ", FLAGS_size);
  fprintf(fp, "Window size %zu, ", FLAGS_window_size);
  fprintf(fp, "kAppUnsigBatch %zu, ", kAppUnsigBatch);
  fprintf(fp, "kAppAllsig %u, ", kAppAllsig);
  fflush(fp);
}

// Record machine throughput
void record_machine_tput(FILE* fp, double total_tput) {
  char timebuf[50];
  hrd_get_formatted_time(timebuf);

  fprintf(fp, "Machine %zu: tput = %.2f reqs/s, time %s\n", FLAGS_machine_id,
          total_tput, timebuf);
  fflush(fp);
}

#endif  // MAIN_H
