#include "main.h"
#include <getopt.h>
#include "hrd.h"

#include <stdint.h>

// Number of QPs used by servers for sends. We use 1 QP for recvs
#define NUM_UD_QPS 3

#define NUM_SERVER_THREADS 2
#define BUF_SIZE 4096
#define CACHELINE_SIZE 64
#define MAX_POSTLIST 128

#define UNSIG_BATCH 64
#define UNSIG_BATCH_ (UNSIG_BATCH - 1)

struct thread_params {
  int id;
  int dual_port;
  int size;
  int postlist;
  int num_threads;

  double* tput;  // Per-thread throughput to compute cumulative tput
};

void* run_server(void* arg);
void* run_client(void* arg);

void* run_client(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;

  // The local HID of a control block should be <= 64 to keep the SHM key low.
  // But the number of clients over all machines can be larger.
  int clt_gid = params.id;  // Global ID of this client thread
  int clt_local_hid = clt_gid % params.num_threads;

  int srv_gid = clt_gid % NUM_SERVER_THREADS;
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_local_hid, ib_port_index, -1,  // port_index, numa_node_id
      0, 0,                              // num conn qps, use uc
      NULL, 0, -1,                       // prealloc conn buf, buf size, key
      1, BUF_SIZE, -1);                  // num_dgram_qps, dgram_buf_size, key

  // Buffer to receive responses into
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  // Buffer to send requests from
  uint8_t* req_buf = malloc(params.size);
  assert(req_buf != 0);
  memset(req_buf, clt_gid, params.size);

  char srv_name[HRD_QP_NAME_SIZE];
  sprintf(srv_name, "server-%d", srv_gid);
  char clt_name[HRD_QP_NAME_SIZE];
  sprintf(clt_name, "client-%d", clt_gid);

  hrd_publish_dgram_qp(cb, 0, clt_name);
  printf("main: Client %s published. Waiting for server %s\n", clt_name,
         srv_name);

  struct hrd_qp_attr* srv_qp = NULL;
  while (srv_qp == NULL) {
    srv_qp = hrd_get_published_qp(srv_name);
    if (srv_qp == NULL) {
      usleep(200000);
    }
  }

  printf("main: Client %s found server! Now posting SENDs.\n", clt_name);

  struct ibv_ah_attr ah_attr = {
      .is_global = 0,
      .dlid = srv_qp->lid,
      .sl = 0,
      .src_path_bits = 0,
      .port_num = cb->dev_port_id,
  };

  struct ibv_ah* ah = ibv_create_ah(cb->pd, &ah_attr);
  assert(ah != NULL);

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_wc wc[MAX_POSTLIST];
  struct ibv_sge sgl[MAX_POSTLIST];
  long long rolling_iter = 0;  // For throughput measurement
  int w_i = 0;                 // Window index
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= K_512) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Client %d: %.2f Mops\n", clt_gid, K_512 / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (w_i = 0; w_i < params.postlist; w_i++) {
      hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf, BUF_SIZE,
                          cb->dgram_buf_mr->lkey);

      wr[w_i].wr.ud.ah = ah;
      wr[w_i].wr.ud.remote_qpn = srv_qp->qpn;
      wr[w_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

      wr[w_i].opcode = IBV_WR_SEND;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = (w_i == 0) ? IBV_SEND_SIGNALED : 0;
      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = (uint64_t)(uintptr_t)req_buf;
      sgl[w_i].length = params.size;

      rolling_iter++;
    }

    ret = ibv_post_send(cb->dgram_qp[0], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    hrd_poll_cq(cb->dgram_send_cq[0], 1, wc);  // Poll SEND (w_i = 0)

    // Poll for all RECVs
    hrd_poll_cq(cb->dgram_recv_cq[0], params.postlist, wc);
  }

  return NULL;
}

void* run_server(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id;  // Global ID of this server thread
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      srv_gid,                    // local_hid
      ib_port_index, -1,          // port_index, numa_node_id
      0, 0,                       // num conn qps, use uc
      NULL, 0, -1,                // prealloc conn buf, buf size, key
      NUM_UD_QPS, BUF_SIZE, -1);  // num_dgram_qps, dgram_buf_size, key

  // Buffer to receive requests into
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  // Buffer to send responses from
  uint8_t* resp_buf = malloc(params.size);
  assert(resp_buf != 0);
  memset(resp_buf, 1, params.size);

  char srv_name[HRD_QP_NAME_SIZE];
  sprintf(srv_name, "server-%d", srv_gid);

  // We post RECVs only on the 1st QP.
  for (i = 0; i < HRD_Q_DEPTH; i++) {
    hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf,
                        params.size + 40,  // Space for GRH
                        cb->dgram_buf_mr->lkey);
  }

  hrd_publish_dgram_qp(cb, 0, srv_name);
  printf("server: Server %s published. Now polling..\n", srv_name);

  // We'll initialize address handles on demand
  struct ibv_ah* ah[HRD_MAX_LID];
  memset(ah, 0, HRD_MAX_LID * sizeof(uintptr_t));

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_recv_wr recv_wr[MAX_POSTLIST], *bad_recv_wr;
  struct ibv_wc wc[MAX_POSTLIST];
  struct ibv_sge sgl[MAX_POSTLIST];
  long long rolling_iter = 0;  // For throughput measurement
  long long nb_tx[NUM_UD_QPS] = {0}, nb_tx_tot = 0, nb_post_send = 0;
  int w_i = 0;  // Window index
  int ud_qp_i = 0;
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  int recv_offset = 0;
  while (recv_offset < params.size + 40) {
    recv_offset += CACHELINE_SIZE;
  }
  assert(recv_offset * params.postlist <= BUF_SIZE);

  while (1) {
    if (rolling_iter >= M_4) {
      clock_gettime(CLOCK_REALTIME, &end);

      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      params.tput[srv_gid] = M_4 / seconds;
      printf("main: Server %d: %.2f Mops. Average postlist = %.2f\n", srv_gid,
             params.tput[srv_gid], (double)nb_tx_tot / nb_post_send);

      if (srv_gid == 0) {
        double total_tput = 0;
        for (i = 0; i < NUM_SERVER_THREADS; i++) {
          total_tput += params.tput[i];
        }
        hrd_red_printf("main: Total = %.2f\n", total_tput);
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &start);
    }

    int num_comps = ibv_poll_cq(cb->dgram_recv_cq[0], params.postlist, wc);
    if (num_comps == 0) {
      continue;
    }

    // Post a batch of RECVs
    for (w_i = 0; w_i < num_comps; w_i++) {
      sgl[w_i].length = params.size + 40;
      sgl[w_i].lkey = cb->dgram_buf_mr->lkey;
      sgl[w_i].addr = (uintptr_t)&cb->dgram_buf[recv_offset * w_i];

      recv_wr[w_i].sg_list = &sgl[w_i];
      recv_wr[w_i].num_sge = 1;
      recv_wr[w_i].next = (w_i == num_comps - 1) ? NULL : &recv_wr[w_i + 1];
    }
    ret = ibv_post_recv(cb->dgram_qp[0], &recv_wr[0], &bad_recv_wr);
    CPE(ret, "ibv_post_recv error", ret);

    for (w_i = 0; w_i < num_comps; w_i++) {
      int s_lid = wc[w_i].slid;  // Src LID for this request
      if (ah[s_lid] == NULL) {
        struct ibv_ah_attr ah_attr = {
            .is_global = 0,
            .dlid = s_lid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = cb->dev_port_id,
        };

        ah[s_lid] = ibv_create_ah(cb->pd, &ah_attr);
        assert(ah[s_lid] != NULL);
      }

      wr[w_i].wr.ud.ah = ah[wc[w_i].slid];
      wr[w_i].wr.ud.remote_qpn = wc[w_i].src_qp;
      wr[w_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

      wr[w_i].opcode = IBV_WR_SEND;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == num_comps - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags =
          ((nb_tx[ud_qp_i] & UNSIG_BATCH_) == 0) ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx[ud_qp_i] & UNSIG_BATCH_) == UNSIG_BATCH_) {
        hrd_poll_cq(cb->dgram_send_cq[ud_qp_i], 1, wc);
      }

      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = (uint64_t)(uintptr_t)resp_buf;
      sgl[w_i].length = params.size;

      nb_tx[ud_qp_i]++;
      nb_tx_tot++;
      rolling_iter++;
    }

    nb_post_send++;
    ret = ibv_post_send(cb->dgram_qp[ud_qp_i], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    HRD_MOD_ADD(ud_qp_i, NUM_UD_QPS);
  }

  return NULL;
}

int main(int argc, char* argv[]) {
  int i, c;
  int num_threads = -1, dual_port = -1;
  int is_client = -1, machine_id = -1, size = -1, postlist = -1;
  double* tput;
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

  // Parse and check arguments
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
  assert(UNSIG_BATCH >= postlist);         // Postlist check
  assert(HRD_Q_DEPTH >= 2 * UNSIG_BATCH);  // Queue capacity check

  if (is_client == 1) {
    assert(num_threads >= 1);
    assert(machine_id >= 0);
  } else {
    assert(num_threads == -1);  // Server runs NUM_SERVER_THREADS threads
    tput = malloc(NUM_SERVER_THREADS * sizeof(double));
    num_threads = NUM_SERVER_THREADS;
    assert(machine_id == -1);
  }

  // Launch a single server thread or multiple client threads
  printf("main: Using %d threads\n", num_threads);
  param_arr = malloc(num_threads * sizeof(struct thread_params));
  thread_arr = malloc(num_threads * sizeof(pthread_t));

  for (i = 0; i < num_threads; i++) {
    param_arr[i].dual_port = dual_port;
    param_arr[i].size = size;
    param_arr[i].postlist = postlist;

    if (is_client) {
      param_arr[i].id = (machine_id * num_threads) + i;
      param_arr[i].num_threads = num_threads;
      pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
    } else {
      tput[i] = 0;  // Initialize this thread's throughput
      param_arr[i].tput = tput;
      param_arr[i].id = i;
      pthread_create(&thread_arr[i], NULL, run_server, &param_arr[i]);
    }
  }

  for (i = 0; i < num_threads; i++) {
    pthread_join(thread_arr[i], NULL);
  }

  return 0;
}
