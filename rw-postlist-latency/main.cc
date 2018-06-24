#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppBufSize = 8192;
static constexpr size_t kAppMaxPostlist = 64;
static constexpr size_t kAppMaxServers = 64;
static constexpr size_t kAppNumLatencySamples = (1024 * 1024);
static constexpr size_t kAppUsePostlist = 1;

DEFINE_uint64(num_threads, 0, "Number of threads");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(use_uc, 0, "Use unreliable connected transport?");
DEFINE_uint64(do_read, 0, "Do RDMA reads?");
DEFINE_uint64(machine_id, 0, "ID of this machine");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(postlist, 0, "Postlist size");

struct thread_params_t {
  int id;
};

void analyse(double* latency_samples) {
  double average_latency = 0;
  for (size_t i = 0; i < kAppNumLatencySamples; i++) {
    average_latency += latency_samples[i];
  }
  average_latency /= kAppNumLatencySamples;

  qsort(latency_samples, kAppNumLatencySamples, sizeof(double), cmpfunc);
  printf("Average latency = %.2fus, median = %.2fus, 99th = %.2fus\n",
         average_latency, latency_samples[kAppNumLatencySamples / 2],
         latency_samples[(kAppNumLatencySamples * 99) / 100]);
}

void run_server(thread_params_t* params) {
  int srv_gid = params->id; /* Global ID of this server thread */
  size_t clt_gid = srv_gid; /* One-to-one connections */
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
                               &conn_config, nullptr);

  struct hrd_ctrl_blk* cb =
      hrd_ctrl_blk_init(srv_gid,            /* local_hid */
                        ib_port_index, -1,  /* port_index, numa_node_id */
                        1, params.use_uc,   /* conn qps, uc */
                        NULL, BUF_SIZE, -1, /* prealloc buf, buf size, key */
                        0, 0, -1);          /* #dgram qps, buf size, shm key */

  memset((void*)cb->conn_buf, (uint8_t)srv_gid + 1, BUF_SIZE);

  char srv_name[kHrdQPNameSize];
  sprintf(srv_name, "server-%d", srv_gid);
  hrd_publish_conn_qp(cb, 0, srv_name);

  printf("main: Server %d published. Waiting for client %d\n", srv_gid,
         clt_gid);

  char clt_name[kHrdQPNameSize];
  struct hrd_qp_attr* clt_qp = NULL;
  sprintf(clt_name, "client-%d", clt_gid);

  while (clt_qp == NULL) {
    clt_qp = hrd_get_published_qp(clt_name);
    if (clt_qp == NULL) {
      usleep(200000);
    }
  }

  hrd_connect_qp(cb, 0, clt_qp);
  hrd_wait_till_ready(clt_name);

  printf("main: Server %d connected!\n", srv_gid);

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_sge sgl[MAX_POSTLIST];
  struct ibv_wc wc;
  long long nb_tx = 0;
  size_t w_i = 0; /* Window index */
  size_t ret;

  // The reads/writes at different postlist positions should be done to/from
  // different cache lines.
  size_t offset = CACHELINE_SIZE;
  while (offset < params.size) {
    offset += CACHELINE_SIZE;
  }
  assert(offset * params.postlist <= BUF_SIZE);

  auto opcode = params.do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);
  auto* latency_samples = new double[kAppNumLatencySamples];
  size_t lat_i = 0; /* Latency sample index */

  while (1) {
    if (lat_i == NUM_LATENCY_SAMPLES) {
      break;
    }

    /* Post a list of work requests in a single ibv_post_send() */
    for (w_i = 0; w_i < params.postlist; w_i++) {
      wr[w_i].opcode = opcode;
      wr[w_i].num_sge = 1;
#if USE_POSTLIST == 1
      wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
#else
      wr[w_i].next = NULL;
#endif
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = (w_i == params.postlist - 1) ? IBV_SEND_SIGNALED : 0;
      if (w_i == 0 && nb_tx != 0) {
        hrd_poll_cq(cb->conn_cq[0], 1, &wc);

        clock_gettime(CLOCK_REALTIME, &end);
        double useconds = (end.tv_sec - start.tv_sec) * 1000000 +
                          (double)(end.tv_nsec - start.tv_nsec) / 1000;

        latency_samples[lat_i] = useconds;
        lat_i++;
        clock_gettime(CLOCK_REALTIME, &start);
      }

      wr[w_i].send_flags |= (params.do_read == 0) ? IBV_SEND_INLINE : 0;

      sgl[w_i].addr = (uint64_t)(uintptr_t)&cb->conn_buf[offset * w_i];
      sgl[w_i].length = params.size;
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;

      wr[w_i].wr.rdma.remote_addr = clt_qp->buf_addr + (offset * w_i);
      wr[w_i].wr.rdma.rkey = clt_qp->rkey;

#if USE_POSTLIST == 0
      ret = ibv_post_send(cb->conn_qp[0], &wr[w_i], &bad_send_wr);
      CPE(ret, "ibv_post_send error", ret);
#endif

      nb_tx++;
    }
#if USE_POSTLIST == 1
    ret = ibv_post_send(cb->conn_qp[0], &wr[0], &bad_send_wr);
#endif
    CPE(ret, "ibv_post_send error", ret);
  }

  analyse(latency_samples);

  return NULL;
}

void* run_client(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;
  size_t clt_gid = params.id; /* Global ID of this server thread */
  size_t srv_gid = clt_gid;   /* One-to-one connections */
  size_t ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_gid,            /* local_hid */
      ib_port_index, -1,  /* port_index, numa_node_id */
      1, params.use_uc,   /* conn qps, uc */
      NULL, BUF_SIZE, -1, /* prealloc conn buf, buf size, key */
      0, 0, -1);          /* #dgram qps, buf size, shm key */

  char srv_name[kHrdQPNameSize];
  sprintf(srv_name, "server-%d", srv_gid);

  char clt_name[kHrdQPNameSize];
  sprintf(clt_name, "client-%d", clt_gid);
  hrd_publish_conn_qp(cb, 0, clt_name);

  printf("main: Client %d published. Waiting for server %d\n", clt_gid,
         srv_gid);

  struct hrd_qp_attr* srv_qp = {NULL};
  while (srv_qp == NULL) {
    srv_qp = hrd_get_published_qp(srv_name);
    if (srv_qp == NULL) {
      usleep(200000);
    }
  }

  hrd_connect_qp(cb, 0, srv_qp);
  hrd_publish_ready(clt_name);

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

  /* Postlist check is trivial because UNSIG_BATCH == postlist*/
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
