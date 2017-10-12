#include "libhrd_cpp/hrd.h"
#include "sweep.h"

// Performance parameters begin

// Include the overhead of memcpy-ing the READ data bc of cacheline versions
#define DO_MEMCPY_ON_READ_COMPLETION 0

// If 1, all RDMA are signaled and UNSIG_BATCH is ignored
#define ALLSIG 1

// If 1, workers increment the shared channel offsets before WRITEs. This
// only makes sense for swarm mode.
#define ENABLE_SHARED_CHANNELS 0

// Performance parameters end

#define MODE_OUTBOUND 0  // Only machine 0 sends RDMA into the swarm
#define MODE_INBOUND 1   // All machines send RDMA to machine 0 only
#define MODE_SWARM 2     // Everyone sends RDMA to everyone

#define ACTIVE_MODE MODE_SWARM  // The current mode

// Conn buf used by workers
#define BUF_SIZE M_2
#define BUF_SIZE_ (BUF_SIZE - 1)

// SHM keys used by workers
#define WORKER_BASE_SHM_KEY 24

// Number of outstanding requests kept by a worker thread  across all QPs.
// This is only used for READs where we can detect completion by polling on
// a READ's destination buffer.
//
// For WRITEs, this is hard to do unless we make every send() signaled. So,
// the number of per-thread outstanding operations per thread with WRITEs is
// O(NUM_CLIENTS * UNSIG_BATCH).
//#define WINDOW_SIZE 44 // Defined in sweep.h

#define NUM_MACHINES 11
#define NUM_VIRTUAL_MACHINES (NUM_MACHINES * VM_PER_MACHINE)
#define NUM_THREADS (NUM_WORKERS / NUM_MACHINES)
#define NUM_QPS_PER_THREAD (NUM_VIRTUAL_MACHINES)

#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

// Upper bounds to avoid dynamic alloc
#define MAX_PORTS 2
#define MAX_MACHINES 256  // Maximum machines in the swarm

#define NUM_CHANNELS 128  // Number of channels a worker writes to
#define NUM_CHANNELS_ (NUM_CHANNELS - 1)
struct channel_offset {
  long long offset;
  long long pad[7];
};

struct thread_params {
  int wrkr_gid;
  int wrkr_lid;
  int machine_id;
  int base_port_index;
  int num_ports;
  int use_uc;
  int size;
  int do_read;

  struct channel_offset* cnoff_arr;
  double* tput_arr;
};

void* run_worker(void* arg);
