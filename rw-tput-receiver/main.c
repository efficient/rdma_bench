#include "hrd.h"
#include <getopt.h>

#define BUF_SIZE (8 * 1024 * 1024)
#define CACHELINE_SIZE 64
#define MAX_POSTLIST 64

#define UNSIG_BATCH 64
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

struct thread_params {
  int id;
  int num_threads; /* Number of client/server threads on this machine */
  int dual_port;
  int use_uc;
  int size;
  int postlist;
  int do_read;
  double* tput_arr;
};

inline uint32_t fastrand(uint64_t* seed) {
  *seed = *seed * 1103515245 + 12345;
  return (uint32_t)(*seed >> 32);
}

void* run_server(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id; /* Global ID of this server thread */
  int clt_gid = srv_gid;   /* One-to-one connections */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      srv_gid,            /* local_hid */
      ib_port_index, -1,  /* port_index, numa_node_id */
      1, params.use_uc,   /* conn qps, use uc */
      NULL, BUF_SIZE, -1, /* prealloc conn buf, buf size, shm key */
      0, 0, -1);          /* #dgram qps, buf size, shm key */

  char srv_name[HRD_QP_NAME_SIZE];
  sprintf(srv_name, "server-%d", srv_gid);
  char clt_name[HRD_QP_NAME_SIZE];
  sprintf(clt_name, "client-%d", clt_gid);

  hrd_publish_conn_qp(cb, 0, srv_name);
  printf("main: Server %s published. Waiting for client %s\n", srv_name,
         clt_name);

  struct hrd_qp_attr* clt_qp = NULL;
  while (clt_qp == NULL) {
    clt_qp = hrd_get_published_qp(clt_name);
    if (clt_qp == NULL) {
      usleep(200000);
    }
  }

  printf("main: Server %s found client! Connecting..\n", srv_name);
  hrd_connect_qp(cb, 0, clt_qp);

  /**< This garbles the server's qp_attr - which is safe */
  hrd_publish_ready(srv_name);
  printf("main: Server %s READY\n", srv_name);

  while (1) {
    printf("main: Server %s: %d\n", srv_name, cb->conn_buf[0]);
    sleep(1);
  }

  return NULL;
}

void* run_client(void* arg) {
  // uint64_t seed = 0xdeadbeef;
  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this client thread */
  int srv_gid = clt_gid;   /* One-to-one connections */
  int clt_lid = params.id % params.num_threads; /* Local ID of this client */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_gid,            /* local_hid */
      ib_port_index, -1,  /* port_index, numa_node_id */
      1, params.use_uc,   /* conn qps, use uc */
      NULL, BUF_SIZE, -1, /* prealloc conn buf, buf size, shm key */
      0, 0, -1);          /* #dgram qps, buf size, shm key */

  memset((void*)cb->conn_buf, (uint8_t)clt_gid + 1, BUF_SIZE);

  char srv_name[HRD_QP_NAME_SIZE];
  sprintf(srv_name, "server-%d", srv_gid);
  char clt_name[HRD_QP_NAME_SIZE];
  sprintf(clt_name, "client-%d", clt_gid);

  hrd_publish_conn_qp(cb, 0, clt_name);
  printf("main: Client %s published. Waiting for server %s\n", clt_name,
         srv_name);

  struct hrd_qp_attr* srv_qp = NULL;
  while (srv_qp == NULL) {
    srv_qp = hrd_get_published_qp(srv_name);
    if (srv_qp == NULL) {
      usleep(200000);
    }
  }

  printf("main: Client %s found server! Connecting..\n", clt_name);
  hrd_connect_qp(cb, 0, srv_qp);

  hrd_wait_till_ready(srv_name);

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_sge sgl[MAX_POSTLIST];
  struct ibv_wc wc;
  long long rolling_iter = 0; /* For performance measurement */
  int w_i = 0;                /* Window index */
  long long nb_tx = 0;        /* For selective signaling */
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
    if (rolling_iter >= M_1) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      double tput = M_1 / seconds;
      printf("main: Client %d: %.2f IOPS\n", clt_gid, tput);
      rolling_iter = 0;

      /* Per-machine stats */
      params.tput_arr[clt_lid] = tput;
      if (clt_lid == 0) {
        double machine_tput = 0;
        int i;
        for (i = 0; i < params.num_threads; i++) {
          machine_tput += params.tput_arr[i];
        }

        hrd_red_printf("main: Machine: %.2f IOPS\n", machine_tput);
      }

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /* Post a postlist of work requests in a single ibv_post_send() */
    for (w_i = 0; w_i < params.postlist; w_i++) {
      wr[w_i].opcode = opcode;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = (nb_tx & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx & UNSIG_BATCH_) == 0 && nb_tx > 0) {
        hrd_poll_cq(cb->conn_cq[0], 1, &wc);
      }

      wr[w_i].send_flags |= (params.do_read == 0) ? IBV_SEND_INLINE : 0;

      sgl[w_i].addr = (uint64_t)(uintptr_t)&cb->conn_buf[offset * w_i];
      sgl[w_i].length = params.size;
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;

      wr[w_i].wr.rdma.remote_addr =
          srv_qp->buf_addr +
          // offset * (fastrand(&seed) % (BUF_SIZE / offset - 1));
          offset * w_i;
      wr[w_i].wr.rdma.rkey = srv_qp->rkey;

      nb_tx++;
    }

    ret = ibv_post_send(cb->conn_qp[0], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    rolling_iter += params.postlist;
  }

  return NULL;
}

int main(int argc, char* argv[]) {
  int i, c;
  int num_threads = -1, dual_port = -1, use_uc = -1; /* Common args */
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

  if (is_client == 1) {
    assert(machine_id >= 0);
    assert(size >= 0);
    if (do_read == 0) {
      assert(size <= HRD_MAX_INLINE);
    }
    assert(postlist >= 1 && postlist <= MAX_POSTLIST);

    assert(UNSIG_BATCH >= postlist);        /* Postlist check */
    assert(HRD_Q_DEPTH >= 2 * UNSIG_BATCH); /* Queue capacity check */

    assert(do_read == 0 || do_read == 1);
  } else {
    assert(machine_id == -1 && size == -1 && postlist == -1 && do_read == -1);
  }

  /* Launch a single server thread or multiple client threads */
  printf("main: Using %d threads\n", num_threads);
  param_arr = malloc(num_threads * sizeof(struct thread_params));
  thread_arr = malloc(num_threads * sizeof(pthread_t));
  double* tput_arr = malloc(num_threads * sizeof(double)); /* For clients */

  for (i = 0; i < num_threads; i++) {
    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      param_arr[i].num_threads = num_threads;
      param_arr[i].dual_port = dual_port;
      param_arr[i].use_uc = use_uc;
      param_arr[i].size = size;
      param_arr[i].do_read = do_read;
      param_arr[i].postlist = postlist;
      param_arr[i].tput_arr = tput_arr;

      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      param_arr[i].num_threads = num_threads;
      param_arr[i].dual_port = dual_port;
      param_arr[i].use_uc = use_uc;

      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
