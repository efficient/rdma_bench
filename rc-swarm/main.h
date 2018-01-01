#ifndef MAIN_H
#define MAIN_H

#include <gflags/gflags.h>
#include <stdlib.h>
#include <array>
#include "libhrd_cpp/hrd.h"

// kAppWindowSize is the number of outstanding requests kept by a worker thread
// across all its QPs. This is only used for READs where we can detect
// completion by polling on a READ's destination buffer.
//
// For WRITEs, this is hard to do unless we make every send() signaled. So,
// the number of per-thread outstanding operations per thread with WRITEs is
// O(qps_per_thread * unsig_batch).

static size_t numa_0_ports[] = {0, 2};
static size_t numa_1_ports[] = {1, 3};

static constexpr size_t kAppBufSize = MB(2);
static_assert(is_power_of_two(kAppBufSize), "");

// Round RDMA source and target offset address to a cacheline boundary. This
// can increase or decrease performance.
static constexpr bool kAppRoundOffset = true;

static constexpr size_t kAppMaxPorts = 2;        // Max ports for a thread
static constexpr size_t kAppMaxProcesses = 256;  // Max processes in the swarm
static constexpr size_t kAppMaxThreads = 28;     // Max threads in a process
static constexpr size_t kAppMaxWindow = 64;      // Max window size
static constexpr int kAppWorkerBaseSHMKey = 24;  // SHM keys used by workers

// Checks
static_assert(kHrdMaxInline == 16, "");   // For single-cacheline WQEs
static_assert(kAppBufSize >= MB(2), "");  // Large buffer, more parallelism

struct thread_params_t {
  size_t wrkr_gid;
  size_t wrkr_lid;
  double* tput_arr;
};

// Flags
DEFINE_uint64(process_id, SIZE_MAX, "Global ID of this process");
DEFINE_uint64(num_processes, SIZE_MAX, "Total processes in cluster");
DEFINE_uint64(num_threads, SIZE_MAX, "Threads per process");
DEFINE_uint64(vms_per_process, SIZE_MAX, "VMs emulated by each process");
DEFINE_uint64(base_port_index, SIZE_MAX, "Base port index");
DEFINE_uint64(numa_node, SIZE_MAX, "NUMA node");
DEFINE_uint64(num_ports, SIZE_MAX, "Number of ports");
DEFINE_uint64(do_read, SIZE_MAX, "Use RDMA READs?");
DEFINE_uint64(size, SIZE_MAX, "RDMA size");
DEFINE_uint64(window_size, SIZE_MAX, "RDMA operation window size");
DEFINE_uint64(allsig, SIZE_MAX, "Signal all post_sends()?");
DEFINE_uint64(sq_depth, SIZE_MAX, "SEND queue depth");
DEFINE_uint64(unsig_batch, SIZE_MAX, "Selective signaling batch");
DEFINE_uint64(max_rd_atomic, SIZE_MAX, "QP's max_rd_atomic");

// File I/O helpers

void record_sweep_params(FILE* fp) {
  fprintf(fp, "Process %zu: sweep parameters: ", FLAGS_process_id);
  fprintf(fp, "Threads per process %zu, ", FLAGS_num_threads);
  fprintf(fp, "RDMA size %zu, ", FLAGS_size);
  fprintf(fp, "Window size %zu, ", FLAGS_window_size);
  fprintf(fp, "kAppUnsigBatch %zu, ", FLAGS_unsig_batch);
  fprintf(fp, "Allsig %zu\n", FLAGS_allsig);
  fflush(fp);
}

void record_process_tput(FILE* fp, double total_tput) {
  char timebuf[50];
  hrd_get_formatted_time(timebuf);

  fprintf(fp, "Process %zu: tput = %.2f reqs/s, time %s\n", FLAGS_process_id,
          total_tput, timebuf);
  fflush(fp);
}

#endif  // MAIN_H
