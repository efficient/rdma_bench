#include <stdint.h>

#define NUM_SERVER_THREADS 28
#define NUM_UD_QPS 1	/* Number of UD QPs used by a server thread for recvs */

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

	double *tput;
};

void *run_server(void *arg);
void *run_client(void *arg);
