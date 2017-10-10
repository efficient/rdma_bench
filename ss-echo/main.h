#include <stdint.h>

/* Number of QPs used by servers for sends. We use 1 QP for recvs */
#define NUM_UD_QPS 3

#define NUM_SERVER_THREADS 2
#define BUF_SIZE 4096
#define CACHELINE_SIZE 64
#define MAX_POSTLIST 128

#define UNSIG_BATCH 64
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

struct thread_params {
  int id;
  int dual_port;
  int size;
  int postlist;
  int num_threads;

  double* tput; /* Per-thread throughput to compute cumulative tput */
};

void* run_server(void* arg);
void* run_client(void* arg);
