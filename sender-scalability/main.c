#include "main.h"
#include <getopt.h>
#include "hrd.h"

int main(int argc, char* argv[]) {
  ct_assert(HRD_Q_DEPTH == 128); /* Too many queues => use small queues */

  ct_assert(NUM_SERVER_THREADS % 2 == 0);
  ct_assert(NUM_CLIENT_THREADS % 2 == 0);
  ct_assert(NUM_CLIENT_THREADS % NUM_CLIENT_MACHINES == 0);

  int i, c;
  int dual_port = -1, use_uc = -1, run_time = DEFAULT_RUN_TIME;
  int is_client = -1, machine_id = -1, size = -1, do_read = -1;
  struct thread_params* param_arr;
  pthread_t* thread_arr;

  static struct option opts[] = {
      {.name = "dual-port", .has_arg = 1, .val = 'd'},
      {.name = "use-uc", .has_arg = 1, .val = 'u'},
      {.name = "run-time", .has_arg = 1, .val = 'R'},
      {.name = "is-client", .has_arg = 1, .val = 'c'},
      {.name = "machine-id", .has_arg = 1, .val = 'm'},
      {.name = "size", .has_arg = 1, .val = 's'},
      {.name = "do-read", .has_arg = 1, .val = 'r'},
      {0}};

  /* Parse and check arguments */
  while (1) {
    c = getopt_long(argc, argv, "t:d:u:c:m:s:w:r", opts, NULL);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'd':
        dual_port = atoi(optarg);
        break;
      case 'u':
        use_uc = atoi(optarg);
        break;
      case 'R':
        run_time = atoi(optarg);
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
      case 'r':
        do_read = atoi(optarg);
        break;
      default:
        printf("Invalid argument %d\n", c);
        assert(false);
    }
  }

  assert(dual_port == 0 || dual_port == 1);
  assert(use_uc == 0 || use_uc == 1);
  assert(run_time == DEFAULT_RUN_TIME || run_time >= 0);
  assert(is_client == 0 || is_client == 1);

  int num_threads;
  if (is_client == 1) {
    num_threads = NUM_CLIENT_THREADS / NUM_CLIENT_MACHINES;
    assert(machine_id >= 0);
    assert(size == -1 && do_read == -1);
  } else {
    num_threads = NUM_SERVER_THREADS;
    assert(machine_id == -1);
    assert(size >= 0);

    if (do_read == 0) {
      assert(size <= HRD_MAX_INLINE);
    }
    assert(do_read == 0 || do_read == 1);

    /* Servers don't use postlist, so we don't need a postlist check */
    assert(HRD_Q_DEPTH >= 2 * UNSIG_BATCH); /* Queue capacity check */
  }

  /* Launch the server or client threads */
  printf("main: Launching %d threads\n", num_threads);
  param_arr = malloc(num_threads * sizeof(struct thread_params));
  thread_arr = malloc(num_threads * sizeof(pthread_t));
  double* tput = malloc(num_threads * sizeof(double));

  for (i = 0; i < num_threads; i++) {
    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      param_arr[i].dual_port = dual_port;
      param_arr[i].run_time = run_time;
      param_arr[i].use_uc = use_uc;

      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      param_arr[i].dual_port = dual_port;
      param_arr[i].run_time = run_time;
      param_arr[i].use_uc = use_uc;

      param_arr[i].size = size;
      param_arr[i].do_read = do_read;
      param_arr[i].tput = tput;

      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
    printf("main: Thread %d done\n", i);
  }

  return 0;
}
