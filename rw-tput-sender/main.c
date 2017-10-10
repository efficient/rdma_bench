#define _GNU_SOURCE /* For CPU_ZERO etc */
#include <getopt.h>
#include "hrd.h"

#define NUM_QPS 1
#define BUF_SIZE 8192
#define CACHELINE_SIZE 64
#define MAX_POSTLIST 64
#define MAX_SERVERS 64

#define UNSIG_BATCH 32
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

double tput[MAX_SERVERS];

struct thread_params {
  int num_threads; /* Number of threads launched on this machine */
  int id;
  int dual_port;
  int use_uc;
  int size;
  int postlist;
  int do_read;
};

int stick_this_thread_to_core(int core_id) {
  int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (core_id < 0 || core_id >= num_cores) return EINVAL;

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pthread_t current_thread = pthread_self();
  return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

void* run_server(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id; /* Global ID of this server thread */
  int clt_gid = srv_gid;   /* One-to-one connections */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  /*
   * Pinning is useful when running servers on 2 sockets - we want to check
   * if socket 0's cores are getting higher tput than socket 1, which is hard
   * if threads move between cores.
   */
  if (params.num_threads > 16) {
    hrd_red_printf(
        "main: Server is running on two sockets. Pinning threads"
        " using CPU_SET\n");
    sleep(2);
    stick_this_thread_to_core(srv_gid);
  }

  struct hrd_ctrl_blk* cb =
      hrd_ctrl_blk_init(srv_gid,                /* local_hid */
                        ib_port_index, -1,      /* port_index, numa_node_id */
                        NUM_QPS, params.use_uc, /* conn qps, uc */
                        NULL, BUF_SIZE, -1, /* prealloc buf, buf size, key */
                        0, 0, -1);          /* #dgram qps, buf size, shm key */

  memset((void*)cb->conn_buf, (uint8_t)srv_gid + 1, BUF_SIZE);

  char srv_name[NUM_QPS][HRD_QP_NAME_SIZE];
  for (i = 0; i < NUM_QPS; i++) {
    sprintf(srv_name[i], "server-%d-%d", srv_gid, i);
    hrd_publish_conn_qp(cb, i, srv_name[i]);
  }
  printf("main: Server %d published. Waiting for client %d\n", srv_gid,
         clt_gid);

  char clt_name[NUM_QPS][HRD_QP_NAME_SIZE];
  struct hrd_qp_attr* clt_qp[NUM_QPS];
  for (i = 0; i < NUM_QPS; i++) {
    sprintf(clt_name[i], "client-%d-%d", clt_gid, i);

    clt_qp[i] = NULL;
    while (clt_qp[i] == NULL) {
      clt_qp[i] = hrd_get_published_qp(clt_name[i]);
      if (clt_qp[i] == NULL) {
        usleep(200000);
      }
    }

    hrd_connect_qp(cb, i, clt_qp[i]);
    hrd_wait_till_ready(clt_name[i]);
  }

  printf("main: Server %d connected!\n", srv_gid);

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_sge sgl[MAX_POSTLIST];
  struct ibv_wc wc;
  long long rolling_iter = 0; /* For performance measurement */
  long long nb_tx[NUM_QPS] = {0};
  int qp_i = 0; /* Round robin between QPs across postlists */
  int w_i = 0;  /* Window index */
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  /*
   * The reads/writes at different postlist positions should be done to/from
   * different cache lines.
   */
  int offset = CACHELINE_SIZE;
  while (offset < params.size) {
    offset += CACHELINE_SIZE;
  }
  assert(offset * params.postlist <= BUF_SIZE);

  int opcode = params.do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  while (1) {
    if (rolling_iter >= M_8) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;

      tput[srv_gid] = M_8 / seconds;
      printf("main: Server %d: %.2f IOPS\n", srv_gid, M_8 / seconds);

      /* Collecting stats at every server can reduce tput by ~ 10% */
      if (srv_gid == 0 && rand() % 5 == 0) {
        double total_tput = 0;
        for (i = 0; i < MAX_SERVERS; i++) {
          total_tput += tput[i];
        }
        hrd_red_printf("main: Total throughput = %.2f\n", total_tput);
      }

      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /* Post a list of work requests in a single ibv_post_send() */
    for (w_i = 0; w_i < params.postlist; w_i++) {
      wr[w_i].opcode = opcode;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags =
          (nb_tx[qp_i] & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx[qp_i] & UNSIG_BATCH_) == 0 && nb_tx[qp_i] != 0) {
        hrd_poll_cq(cb->conn_cq[qp_i], 1, &wc);
      }

      wr[w_i].send_flags |= (params.do_read == 0) ? IBV_SEND_INLINE : 0;

      sgl[w_i].addr = (uint64_t)(uintptr_t)&cb->conn_buf[offset * w_i];
      sgl[w_i].length = params.size;
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;

      wr[w_i].wr.rdma.remote_addr = clt_qp[qp_i]->buf_addr + (offset * w_i);
      wr[w_i].wr.rdma.rkey = clt_qp[qp_i]->rkey;

      nb_tx[qp_i]++;
    }

    ret = ibv_post_send(cb->conn_qp[qp_i], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    rolling_iter += params.postlist;

    /* Use a different QP for the next postlist */
    qp_i++;
    if (qp_i == NUM_QPS) {
      qp_i = 0;
    }
  }

  return NULL;
}

void* run_client(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this server thread */
  int srv_gid = clt_gid;   /* One-to-one connections */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb =
      hrd_ctrl_blk_init(clt_gid,                /* local_hid */
                        ib_port_index, -1,      /* port_index, numa_node_id */
                        NUM_QPS, params.use_uc, /* conn qps, uc */
                        NULL, BUF_SIZE, -1, /* prealloc buf, buf size, key */
                        0, 0, -1);          /* #dgram qps, buf size, shm key */

  char srv_name[NUM_QPS][HRD_QP_NAME_SIZE];
  for (i = 0; i < NUM_QPS; i++) {
    sprintf(srv_name[i], "server-%d-%d", srv_gid, i);
  }

  char clt_name[NUM_QPS][HRD_QP_NAME_SIZE];
  for (i = 0; i < NUM_QPS; i++) {
    sprintf(clt_name[i], "client-%d-%d", clt_gid, i);
    hrd_publish_conn_qp(cb, i, clt_name[i]);
  }

  printf("main: Client %d published. Waiting for server %d\n", clt_gid,
         srv_gid);

  struct hrd_qp_attr* srv_qp[NUM_QPS] = {NULL};
  for (i = 0; i < NUM_QPS; i++) {
    while (srv_qp[i] == NULL) {
      srv_qp[i] = hrd_get_published_qp(srv_name[i]);
      if (srv_qp[i] == NULL) {
        usleep(200000);
      }
    }

    hrd_connect_qp(cb, i, srv_qp[i]);
    hrd_publish_ready(clt_name[i]);
  }

  printf("main: Client %d connected!\n", clt_gid);

  while (1) {
    printf("main: Client %d: %d\n", clt_gid, cb->conn_buf[0]);
    sleep(1);
  }
}

int main(int argc, char* argv[]) {
  int i, c;
  int num_threads = -1, dual_port = -1, use_uc = -1;
  int is_client = -1, machine_id = -1, size = -1, postlist = -1, do_read = -1;
  struct thread_params* param_arr;
  pthread_t* thread_arr;

  static struct option opts[] = {
      {.name = "num-threads", .has_arg = 1, .val = 't'},
      {.name = "dual-port", .has_arg = 1, .val = 'd'},
      {.name = "use-uc", .has_arg = 1, .val = 'u'},
      {.name = "is-client", .has_arg = 1, .val = 'c'},
      {.name = "machine-id", .has_arg = 1, .val = 'm'},
      {.name = "size", .has_arg = 1, .val = 's'},
      {.name = "postlist", .has_arg = 1, .val = 'p'},
      {.name = "do-read", .has_arg = 1, .val = 'r'},
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
      case 'p':
        postlist = atoi(optarg);
        break;
      case 'r':
        do_read = atoi(optarg);
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

  assert(UNSIG_BATCH >= postlist);     /* Postlist check */
  assert(HRD_Q_DEPTH >= 2 * postlist); /* Queue capacity check */

  if (is_client == 1) {
    assert(machine_id >= 0);
    assert(size == -1 && postlist == -1 && do_read == -1);
  } else {
    assert(machine_id == -1);
    assert(size >= 0);
    if (do_read == 0) { /* We always do inlined WRITEs */
      assert(size <= HRD_MAX_INLINE);
    }
    assert(postlist >= 1 && postlist <= MAX_POSTLIST);
    assert(do_read == 0 || do_read == 1);
  }

  for (i = 0; i < MAX_SERVERS; i++) {
    tput[i] = 0;
  }

  printf("main: Using %d threads\n", num_threads);
  param_arr = malloc(num_threads * sizeof(struct thread_params));
  thread_arr = malloc(num_threads * sizeof(pthread_t));

  for (i = 0; i < num_threads; i++) {
    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      param_arr[i].dual_port = dual_port;
      param_arr[i].use_uc = use_uc;

      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      param_arr[i].num_threads = num_threads;
      param_arr[i].id = i;
      param_arr[i].dual_port = dual_port;
      param_arr[i].use_uc = use_uc;

      param_arr[i].size = size;
      param_arr[i].do_read = do_read;
      param_arr[i].postlist = postlist;

      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
