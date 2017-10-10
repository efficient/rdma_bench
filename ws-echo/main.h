#include <stdint.h>

#define RR_SIZE (2 * 1024 * 1024) /* Request region size */
#define CACHELINE_SIZE 64
#define MAX_POSTLIST 128

#define NUM_WORKERS 5
#define NUM_CLIENTS 64

#define USE_UC 1
#define UNSIG_BATCH 256
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

/* SHM key for the 1st request region created by master. ++ for other RRs.*/
#define MASTER_SHM_KEY 24

#define LL long long
#define SLOT_SIZE_LL (CACHELINE_SIZE / sizeof(LL))

struct thread_params {
  int id;
  int dual_port;
  int size;
  int postlist;
};

void* run_master(void* arg);
void* run_worker(void* arg);
void* run_client(void* arg);
