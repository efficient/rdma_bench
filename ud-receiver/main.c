#include "main.h"
#include <getopt.h>
#include "hrd.h"

int main(int argc, char* argv[]) {
  int i, c;
  int num_threads = -1, dual_port = -1;
  int is_client = -1, machine_id = -1, size = -1, postlist = -1;
  struct thread_params* param_arr;
  pthread_t* thread_arr;

  static struct option opts[] = {
      {.name = "num-threads", .has_arg = 1, .val = 't'},
      {.name = "dual-port", .has_arg = 1, .val = 'd'},
      {.name = "is-client", .has_arg = 1, .val = 'c'},
      {.name = "machine-id", .has_arg = 1, .val = 'm'},
      {.name = "size", .has_arg = 1, .val = 's'},
      {.name = "postlist", .has_arg = 1, .val = 'p'},
      {0}};

  /* Parse and check arguments */
  while (1) {
    c = getopt_long(argc, argv, "t:d:u:c:m:s:w:r", opts, NULL);
    if (c == -1) {
      break;
    }
    switch (c) {
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

  assert(dual_port == 0 || dual_port == 1);
  assert(is_client == 0 || is_client == 1);
  assert(size >= 0 && size <= HRD_MAX_INLINE);
  assert(postlist >= 1 && postlist <= MAX_POSTLIST);

  if (is_client == 1) {
    /*
     * Poll for completed send()s only after performing a signaled send().
     * This check is not needed for server because server does not send().
     */
    assert(UNSIG_BATCH >= 2 * postlist);
    assert(machine_id >= 0);
    assert(num_threads >= 1);
  } else {
    /* Don't post too many RECVs */
    assert(postlist <= HRD_Q_DEPTH / 2);
    assert(machine_id == -1);
    assert(num_threads == -1);
    num_threads = NUM_SERVER_THREADS;
  }

  /* Launch a single server thread or multiple client threads */
  printf("main: Using %d threads\n", num_threads);
  param_arr = malloc(num_threads * sizeof(struct thread_params));
  thread_arr = malloc(num_threads * sizeof(pthread_t));
  double* tput = malloc(num_threads * sizeof(double));

  for (i = 0; i < num_threads; i++) {
    param_arr[i].dual_port = dual_port;
    param_arr[i].size = size;
    param_arr[i].postlist = postlist;

    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      param_arr[i].num_threads = num_threads;
      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      param_arr[i].tput = tput;
      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
