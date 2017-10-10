#include "main.h"
#include <getopt.h>
#include "hrd.h"

int main(int argc, char* argv[]) {
  int i, c;
  int num_threads = -1, dual_port = -1, use_uc = -1;
  int is_client = -1, machine_id = -1, size = -1, window = -1;
  struct thread_params* param_arr;
  pthread_t* thread_arr;

  static struct option opts[] = {
      {.name = "num-threads", .has_arg = 1, .val = 't'},
      {.name = "dual-port", .has_arg = 1, .val = 'd'},
      {.name = "use-uc", .has_arg = 1, .val = 'u'},
      {.name = "is-client", .has_arg = 1, .val = 'c'},
      {.name = "machine-id", .has_arg = 1, .val = 'm'},
      {.name = "size", .has_arg = 1, .val = 's'},
      {.name = "window", .has_arg = 1, .val = 'w'},
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
      case 'u':
        use_uc = atoi(optarg);
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
      case 'w':
        window = atoi(optarg);
        break;
      default:
        printf("Invalid argument %d\n", c);
        assert(false);
    }
  }

  assert(num_threads >= 1);
  assert(dual_port == 0 || dual_port == 1);
  assert(use_uc == 0 || use_uc == 1);
  assert(is_client == 0 || is_client == 1);
  assert(size >= 1 && size <= BUF_SIZE);
  assert(window >= 1 && window <= MAX_WINDOW_SIZE);

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
    param_arr[i].use_uc = use_uc;
    param_arr[i].size = size;
    param_arr[i].window = window;

    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
