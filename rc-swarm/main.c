#include "main.h"
#include <getopt.h>

int main(int argc, char* argv[]) {
  assert(BUF_SIZE >= M_2); /* Large buffer, more parallelism */
  if (ENABLE_SHARED_CHANNELS == 1) {
    assert(ACTIVE_MODE == MODE_SWARM);
  }

  assert(NUM_MACHINES >= 2);               /* At least 2 machines */
  assert(NUM_WORKERS % NUM_MACHINES == 0); /* For NUM_THREADS */
  assert(HRD_Q_DEPTH == 128); /* Use small queues to reduce cache pressure */
  assert(HRD_Q_DEPTH >= 2 * UNSIG_BATCH); /* Queue capacity check */
  /* No postlist check because RC workers don't use postlist */

  int i, c;
  int machine_id = -1, size = SIZE, use_uc = -1, do_read = -1;
  int base_port_index = -1, num_ports = -1; /* Same across all swarmhosts */

  static struct option opts[] = {
      {.name = "use-uc", .has_arg = 1, .val = 'u'},
      {.name = "base-port-index", .has_arg = 1, .val = 'b'},
      {.name = "num-ports", .has_arg = 1, .val = 'N'},
      {.name = "machine-id", .has_arg = 1, .val = 'm'},
      {.name = "size", .has_arg = 1, .val = 's'},
      {.name = "do-read", .has_arg = 1, .val = 'r'},
      {0}};

  /* Parse and check arguments */
  while (1) {
    c = getopt_long(argc, argv, "t:u:b:N:m:s", opts, NULL);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'u':
        use_uc = atoi(optarg);
        break;
      case 'b':
        base_port_index = atoi(optarg);
        break;
      case 'N':
        num_ports = atoi(optarg);
        break;
      case 'm':
        machine_id = atoi(optarg);
        break;
      // case 's':
      //	size = atoi(optarg);
      //	break;
      case 'r':
        do_read = atoi(optarg);
        break;
      default:
        printf("Invalid argument %d\n", c);
        assert(false);
    }
  }

  /* Common checks */
  assert(use_uc == 0 || use_uc == 1);
  assert(base_port_index >= 0 && base_port_index <= 8);
  assert(num_ports >= 1 && num_ports <= MAX_PORTS);
  assert(machine_id >= 0);
  assert(do_read == 0 || do_read == 1);

  /* Only WRITEs need to share remote channels */
  if (ENABLE_SHARED_CHANNELS == 1) {
    assert(do_read == 0);
  }

  /* Size checks based on opcode. This is not UD, so no 0-byte read/write. */
  if (do_read == 0) { /* For WRITEs */
    assert(size >= 1 && size <= HRD_MAX_INLINE);
  } else {
    /* WINDOW_SIZE reads are kept outstanding over ALL QPs. */
    assert(size >= 1 && size <= BUF_SIZE);
  }

  /* Create shared arrays for workers */
  double* tput_arr = malloc(NUM_THREADS * sizeof(double));
  struct channel_offset* cnoff_arr = malloc(NUM_THREADS * sizeof(*cnoff_arr));
  for (i = 0; i < NUM_THREADS; i++) {
    tput_arr[i] = 0.0;
    cnoff_arr[i].offset = 0;
  }

  struct thread_params* param_arr =
      malloc(NUM_THREADS * sizeof(struct thread_params));
  pthread_t* thread_arr = malloc(NUM_THREADS * sizeof(pthread_t));

  /* Launch workers */
  printf("main: Launching %d swarm workers\n", NUM_THREADS);

  for (i = 0; i < NUM_THREADS; i++) {
    param_arr[i].wrkr_gid = (machine_id * NUM_THREADS) + i;
    param_arr[i].wrkr_lid = i;
    param_arr[i].machine_id = machine_id;
    param_arr[i].tput_arr = tput_arr;
    param_arr[i].cnoff_arr = cnoff_arr;

    param_arr[i].base_port_index = base_port_index;
    param_arr[i].num_ports = num_ports;
    param_arr[i].use_uc = use_uc;
    param_arr[i].do_read = do_read;
    param_arr[i].size = SIZE;

    pthread_create(&thread_arr[i], NULL, run_worker, &param_arr[i]);
  }

  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
