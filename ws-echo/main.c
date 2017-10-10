#include "main.h"
#include <getopt.h>
#include "hrd.h"

int main(int argc, char* argv[]) {
  /*
   * UNSIG_BATCH and number of outstanding requests maintained by clients
   * depends on queue depth being large.
   */
  assert(HRD_Q_DEPTH == 1024);

  int i, c;
  int is_master = -1;
  int num_threads = -1, dual_port = -1;
  int is_client = -1, machine_id = -1, size = -1, postlist = -1;
  struct thread_params* param_arr;
  pthread_t* thread_arr;

  static struct option opts[] = {
      {.name = "master", .has_arg = 1, .val = 'M'},
      {.name = "num-threads", .has_arg = 1, .val = 't'},
      {.name = "dual-port", .has_arg = 1, .val = 'd'},
      {.name = "use-uc", .has_arg = 1, .val = 'u'},
      {.name = "is-client", .has_arg = 1, .val = 'c'},
      {.name = "machine-id", .has_arg = 1, .val = 'm'},
      {.name = "size", .has_arg = 1, .val = 's'},
      {.name = "postlist", .has_arg = 1, .val = 'p'},
      {0}};

  /* All requests should fit into the master's request region */
  assert(CACHELINE_SIZE * NUM_CLIENTS * NUM_WORKERS < RR_SIZE);

  /* Parse and check arguments */
  while (1) {
    c = getopt_long(argc, argv, "M:t:d:u:c:m:s:w:r", opts, NULL);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'M':
        is_master = atoi(optarg);
        assert(is_master == 1);
        break;
      case 't':
        num_threads = atoi(optarg);
        break;
      case 'd':
        dual_port = atoi(optarg);
        break;
      case 'c':
        is_client = atoi(optarg);
        break;
      case 'm':
        machine_id = atoi(optarg);
        break;
      case 's':
        size = atoi(optarg);
        break;
      case 'p':
        postlist = atoi(optarg);
        break;
      default:
        printf("Invalid argument %d\n", c);
        assert(false);
    }
  }

  /* Handle the master process specially */
  if (is_master == 1) {
    assert(dual_port == 0 || dual_port == 1);
    struct thread_params master_params;
    pthread_t master_thread;

    master_params.dual_port = dual_port;
    pthread_create(&master_thread, NULL, run_master, &master_params);
    pthread_join(master_thread, NULL);
    exit(0);
  }

  /* Sanity checks for worker process and client machines */
  assert(num_threads >= 1);
  assert(dual_port == 0 || dual_port == 1);
  assert(is_client == 0 || is_client == 1);
  /* TODO: Larger sizes */
  assert(size >= 1 && size <= 24 && size % sizeof(long long) == 0);
  assert(postlist >= 1 && postlist <= MAX_POSTLIST);

  if (is_client == 1) {
    assert(machine_id >= 0);
  } else {
    assert(machine_id == -1);
  }

  /* Launch a single server thread or multiple client threads */
  printf("main: Using %d threads\n", num_threads);
  param_arr = malloc(num_threads * sizeof(struct thread_params));
  thread_arr = malloc(num_threads * sizeof(pthread_t));

  for (i = 0; i < num_threads; i++) {
    param_arr[i].dual_port = dual_port;
    param_arr[i].size = size;
    param_arr[i].postlist = postlist;

    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      pthread_create(&thread_arr[i], NULL, run_worker, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
