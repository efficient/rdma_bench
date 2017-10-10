#define _GNU_SOURCE /* For CPU_ZERO etc */
#include <arpa/inet.h>
#include <byteswap.h>
#include <getopt.h>
#include "hrd.h"

#define BUF_SIZE M_2
#define CACHELINE_SIZE 64
#define MAX_WINDOW 512
#define MAX_SERVERS 64

double tput[MAX_SERVERS];

struct thread_params {
  int num_threads; /* Number of threads launched on this machine */
  int id;
  int dual_port;
  int use_uc;
  int size;
  int window;
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
      hrd_ctrl_blk_init(srv_gid,            /* local_hid */
                        ib_port_index, 0,   /* port_index, numa_node_id */
                        1, params.use_uc,   /* conn qps, uc */
                        NULL, BUF_SIZE, 24, /* prealloc buf, buf size, key */
                        0, 0, -1);          /* #dgram qps, buf size, shm key */

  memset((void*)cb->conn_buf, (uint8_t)srv_gid + 1, BUF_SIZE);

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

  hrd_wait_till_ready(clt_name);

  struct ibv_send_wr wr[MAX_WINDOW], *bad_send_wr;
  struct ibv_sge sgl[MAX_WINDOW];
  struct ibv_wc wc;
  long long rolling_iter = 0; /* For performance measurement */
  long long nb_tx = 0;
  long long nb_test = 0, nb_changed = 0;
  int w_i = 0; /* Window index */
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  int opcode = params.do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  /* The response for the ith request in window is scattered to w_i * 64 */
  assert(params.window * 64 <= BUF_SIZE);

  /*
   * Ensure that read response for ith request in window cannot overlap with
   * request i + 1 even after we garble sge.
   */
  assert(params.size <= 64);

  while (1) {
    if (rolling_iter >= K_512) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;

      tput[srv_gid] = K_512 / seconds;
      printf("main: Server %d: %.2f IOPS\n", srv_gid, K_512 / seconds);

      /* Collecting stats at every server can reduce tput by ~ 10% */
      if (srv_gid == 0) {
        double total_tput = 0;
        for (i = 0; i < MAX_SERVERS; i++) {
          total_tput += tput[i];
        }
        hrd_red_printf("main: Total throughput = %.2f\n", total_tput);
        hrd_red_printf("\tmain: Change/s = %.5f\n",
                       ((double)nb_changed / nb_test) * total_tput);
      }

      rolling_iter = 0;
      nb_test = 0;
      nb_changed = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /* params.size <= 64 */
    memset((char*)cb->conn_buf, 0, params.window * 64);

    /* Post a window of work requests without postlist */
    for (w_i = 0; w_i < params.window; w_i++) {
      wr[w_i].opcode = opcode;
      wr[w_i].num_sge = 1;
      wr[w_i].next = NULL;
      wr[w_i].sg_list = &sgl[w_i];

      if (w_i == params.window - 1) {
        wr[w_i].send_flags = IBV_SEND_SIGNALED;
      } else {
        wr[w_i].send_flags = 0;
      }

      wr[w_i].send_flags |= (params.do_read == 0) ? IBV_SEND_INLINE : 0;

      sgl[w_i].addr = (uint64_t)(uintptr_t)&cb->conn_buf[w_i * 64];
      sgl[w_i].length = params.size;
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;
      // printf("Addr = %llx\n", (long long) bswap_64(sgl[w_i].addr));
      // printf("lkey = %d\n", htonl(sgl[w_i].lkey));

      wr[w_i].wr.rdma.remote_addr = clt_qp->buf_addr;
      wr[w_i].wr.rdma.rkey = clt_qp->rkey;

      nb_tx++;

      ret = ibv_post_send(cb->conn_qp[0], &wr[w_i], &bad_send_wr);
      CPE(ret, "ibv_post_send error", ret);
    }

    hrd_poll_cq(cb->conn_cq[0], 1, &wc);

    rolling_iter += params.window;
  }

  return NULL;
}

void* run_client(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this server thread */
  int srv_gid = clt_gid;   /* One-to-one connections */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb =
      hrd_ctrl_blk_init(clt_gid,            /* local_hid */
                        ib_port_index, -1,  /* port_index, numa_node_id */
                        1, params.use_uc,   /* conn qps, uc */
                        NULL, BUF_SIZE, -1, /* prealloc buf, buf size, key */
                        0, 0, -1);          /* #dgram qps, buf size, shm key */

  memset((char*)cb->conn_buf, 1, BUF_SIZE);

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
  hrd_publish_ready(clt_name);
  printf("main: Client %s READY\n", clt_name);

  while (1) {
    printf("main: Client %s: %d\n", clt_name, cb->conn_buf[0]);
    sleep(1);
  }
}

int main(int argc, char* argv[]) {
  int i, c;
  int num_threads = -1, dual_port = -1, use_uc = -1;
  int is_client = -1, machine_id = -1, size = -1, window = -1, do_read = -1;
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
      case 'w':
        window = atoi(optarg);
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
    assert(size == -1 && window == -1 && do_read == -1);
  } else {
    assert(machine_id == -1);
    assert(size >= 0);
    if (do_read == 0) { /* We always do inlined WRITEs */
      assert(size <= HRD_MAX_INLINE);
    }

    /* Post at most HRD_Q_DEPTH / 2 unsignaled ops before polling CQ */
    assert(window <= HRD_Q_DEPTH / 2);

    assert(window >= 1 && window <= MAX_WINDOW);
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
      param_arr[i].window = window;

      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
