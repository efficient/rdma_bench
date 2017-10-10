#include <stdint.h>

#define NUM_UD_QPS 3
#define NUM_SERVER_THREADS 10

#define DISABLE_SERVER_POSTLIST 0

/*
 * The servers' RECV queue will be provisioned for MAX_NUM_CLIENTS clients,
 * but fewer clients can be used.
 */
#define MAX_NUM_CLIENTS 70

#define BUF_SIZE 4096
#define CACHELINE_SIZE 64
#define MAX_POSTLIST 128

#define UNSIG_BATCH 64
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

struct thread_params {
  int id;
  int dual_port;
  int postlist;
  int num_threads;

  double* tput; /* Per-thread throughput to compute cumulative tput */
  uint64_t* counter;
};

void* run_server(void* arg);
void* run_client(void* arg);
