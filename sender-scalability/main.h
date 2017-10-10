/* Conn buf used by server */
#define BUF_SIZE M_2
#define BASE_SHM_KEY 2

#define DEFAULT_RUN_TIME 1000000 /* Just to large to actually run */
#define RUN_TIME_SLACK 10        /* Clients shouldn't die before server */

/*
 * Number of outstanding requests kept by a server thread across all QPs.
 * This is only used for READs where we can detect completion by polling on
 * a READ's destination buffer.
 *
 * For WRITEs, this is hard to do unless we make every send() signaled. So,
 * the number of per-thread outstanding operations per thread with WRITEs is
 * O(NUM_CLIENTS * UNSIG_BATCH).
 */
#define WINDOW_SIZE 16

#define UNSIG_BATCH 4
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

/* Sweep paramaters */
#define NUM_SERVER_THREADS 14
#define NUM_CLIENT_THREADS 80 /* Total client QPs in cluster */
#define NUM_CLIENT_MACHINES 5

struct thread_params {
  int id;
  int dual_port;
  int run_time;
  int use_uc;
  int size;
  int do_read;

  double* tput;
};

void* run_server(void* arg);
void* run_client(void* arg);
