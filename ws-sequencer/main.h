#include <stdint.h>

#define CACHELINE_SIZE 64
#define MAX_POSTLIST 128

/* Configuration options */
#define MAX_SERVER_PORTS 4
#define NUM_WORKERS 6
#define NUM_CLIENTS 70

/* Performance options */
#define WINDOW_SIZE 32 /* Outstanding requests kept by each client */
#define NUM_UD_QPS 3   /* Number of UD QPs per port */
#define USE_POSTLIST 1

#define UNSIG_BATCH 64 /* XXX Check if increasing this helps */
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

/* SHM key for the 1st request region created by master. ++ for other RRs.*/
#define MASTER_SHM_KEY 24
#define RR_SIZE (2 * 1024 * 1024) /* Request region size */
#define OFFSET(wn, cn, ws) \
  ((wn * NUM_CLIENTS * WINDOW_SIZE) + (cn * WINDOW_SIZE) + ws)

struct thread_params {
  int id;
  int base_port_index;
  int num_server_ports;
  int num_client_ports;
  int postlist;
  long long* counter;
};

void* run_master(void* arg);
void* run_worker(void* arg);
void* run_client(void* arg);
