#include "main.h"
#include <getopt.h>
#include "hrd.h"

void* run_server(void* arg) {
  int i;
  int ud_qp_i = 0;  // UD QP index
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id;  // Global ID of this server thread
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      srv_gid,                    // local_hid
      ib_port_index, -1,          // port_index, numa_node_id
      0, 0,                       // conn qps, use uc
      NULL, 0, -1,                // prealloc conn buf, conn buf size, key
      NUM_UD_QPS, BUF_SIZE, -1);  // num_dgram_qps, dgram_buf_size, key

  // Buffer to receive requests into
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  for (ud_qp_i = 0; ud_qp_i < NUM_UD_QPS; ud_qp_i++) {
    // Fill this QP with recvs before publishing it to clients
    for (i = 0; i < HRD_Q_DEPTH; i++) {
      hrd_post_dgram_recv(cb->dgram_qp[ud_qp_i], (void*)cb->dgram_buf,
                          params.size + 40,  // Space for GRH
                          cb->dgram_buf_mr->lkey);
    }

    char srv_name[HRD_QP_NAME_SIZE];
    sprintf(srv_name, "server-%d-%d", srv_gid, ud_qp_i);

    hrd_publish_dgram_qp(cb, ud_qp_i, srv_name);
  }
  ud_qp_i = 0;

  printf("server: Server %d published QPs. Now polling..\n", srv_gid);

  struct ibv_recv_wr recv_wr[MAX_POSTLIST], *bad_recv_wr;
  struct ibv_wc wc[MAX_POSTLIST];
  struct ibv_sge sgl[MAX_POSTLIST];
  long long rolling_iter = 0;  // For throughput measurement
  int w_i = 0;                 // Window index
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  int recv_offset = 0;
  while (recv_offset < params.size + 40) {
    recv_offset += CACHELINE_SIZE;
  }
  assert(recv_offset * params.postlist <= BUF_SIZE);

  while (1) {
    if (rolling_iter >= M_8) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Server %d: %.2f IOPS. \n", srv_gid, rolling_iter / seconds);

      params.tput[srv_gid] = rolling_iter / seconds;
      if (srv_gid == 0) {
        double total_tput = 0;
        for (i = 0; i < NUM_SERVER_THREADS; i++) {
          total_tput += params.tput[i];
        }
        hrd_red_printf("main: Total tput %.2f IOPS.\n", total_tput);
      }

      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    int num_comps =
        ibv_poll_cq(cb->dgram_recv_cq[ud_qp_i], params.postlist, wc);
    if (num_comps == 0) {
      continue;
    }

    // Post a batch of RECVs
    for (w_i = 0; w_i < num_comps; w_i++) {
      assert(wc[w_i].imm_data == 3185);
      sgl[w_i].length = params.size + 40;
      sgl[w_i].lkey = cb->dgram_buf_mr->lkey;
      sgl[w_i].addr = (uintptr_t)&cb->dgram_buf[0];

      recv_wr[w_i].sg_list = &sgl[w_i];
      recv_wr[w_i].num_sge = 1;
      recv_wr[w_i].next = (w_i == num_comps - 1) ? NULL : &recv_wr[w_i + 1];
    }

    ret = ibv_post_recv(cb->dgram_qp[ud_qp_i], &recv_wr[0], &bad_recv_wr);
    CPE(ret, "ibv_post_recv error", ret);

    rolling_iter += num_comps;
    HRD_MOD_ADD(ud_qp_i, NUM_UD_QPS);
  }

  return NULL;
}

void* run_client(void* arg) {
  int ud_qp_i = 0;
  struct thread_params params = *(struct thread_params*)arg;

  // The local HID of a control block should be <= 64 to keep the SHM key low.
  // But the number of clients over all machines can be larger.
  int clt_gid = params.id;  // Global ID of this client thread
  int clt_local_hid = clt_gid % params.num_threads;

  int srv_gid = clt_gid % NUM_SERVER_THREADS;
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_local_hid, ib_port_index, -1,  // port_index, numa_node_id
      0, 0,                              // conn qps, use uc
      NULL, 0, -1,       // prealloc conn buf, conn buf size, key
      1, BUF_SIZE, -1);  // num_dgram_qps, dgram_buf_size, key

  // Buffer to receive responses into
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  // Buffer to send requests from
  uint8_t* req_buf = malloc(params.size);
  assert(req_buf != 0);
  memset(req_buf, clt_gid, params.size);

  printf("main: Client %d waiting for server %d\n", clt_gid, srv_gid);

  struct hrd_qp_attr* srv_qp[NUM_UD_QPS] = {NULL};
  for (ud_qp_i = 0; ud_qp_i < NUM_UD_QPS; ud_qp_i++) {
    char srv_name[HRD_QP_NAME_SIZE];
    sprintf(srv_name, "server-%d-%d", srv_gid, ud_qp_i);
    while (srv_qp[ud_qp_i] == NULL) {
      srv_qp[ud_qp_i] = hrd_get_published_qp(srv_name);
      if (srv_qp[ud_qp_i] == NULL) {
        usleep(200000);
      }
    }
  }
  ud_qp_i = 0;

  printf("main: Client %d found server! Now posting SENDs.\n", clt_gid);

  // We need only 1 ah because a client contacts only 1 server
  struct ibv_ah_attr ah_attr = {
      .is_global = 0,
      .dlid = srv_qp[0]->lid,  // All srv_qp have same LID
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
  long long nb_tx = 0;
  int w_i = 0;  // Window index
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= M_2) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Client %d: %.2f IOPS\n", clt_gid, rolling_iter / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (w_i = 0; w_i < params.postlist; w_i++) {
      wr[w_i].wr.ud.ah = ah;
      wr[w_i].wr.ud.remote_qpn = srv_qp[ud_qp_i]->qpn;
      wr[w_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

      wr[w_i].opcode = IBV_WR_SEND_WITH_IMM;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].imm_data = 3185;
      wr[w_i].sg_list = &sgl[w_i];

      // UNSIG_BATCH >= 2 * postlist ensures that we poll for a
      // completed send() only after we have performed a signaled send().
      wr[w_i].send_flags = (nb_tx & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx & UNSIG_BATCH_) == UNSIG_BATCH_) {
        hrd_poll_cq(cb->dgram_send_cq[0], 1, wc);
      }

      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = (uint64_t)(uintptr_t)req_buf;
      sgl[w_i].length = params.size;

      rolling_iter++;
      nb_tx++;
    }

    ret = ibv_post_send(cb->dgram_qp[0], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    HRD_MOD_ADD(ud_qp_i, NUM_UD_QPS);
  }

  return NULL;
}

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

  if (is_client == 1) {
    // Poll for completed send()s only after performing a signaled send().
    // This check is not needed for server because server does not send().
    assert(UNSIG_BATCH >= 2 * postlist);
    assert(machine_id >= 0);
    assert(num_threads >= 1);
  } else {
    // Don't post too many RECVs
    assert(postlist <= HRD_Q_DEPTH / 2);
    assert(machine_id == -1);
    assert(num_threads == -1);
    num_threads = NUM_SERVER_THREADS;
  }

  // Launch a single server thread or multiple client threads
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
