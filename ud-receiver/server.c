#include "hrd.h"
#include "main.h"

void* run_server(void* arg) {
  int i;
  int ud_qp_i = 0; /* UD QP index */
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id; /* Global ID of this server thread */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      srv_gid,                   /* local_hid */
      ib_port_index, -1,         /* port_index, numa_node_id */
      0, 0,                      /* conn qps, use uc */
      NULL, 0, -1,               /* prealloc conn buf, conn buf size, key */
      NUM_UD_QPS, BUF_SIZE, -1); /* num_dgram_qps, dgram_buf_size, key */

  /* Buffer to receive requests into */
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  for (ud_qp_i = 0; ud_qp_i < NUM_UD_QPS; ud_qp_i++) {
    /* Fill this QP with recvs before publishing it to clients */
    for (i = 0; i < HRD_Q_DEPTH; i++) {
      hrd_post_dgram_recv(cb->dgram_qp[ud_qp_i], (void*)cb->dgram_buf,
                          params.size + 40, /* Space for GRH */
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
  long long rolling_iter = 0; /* For throughput measurement */
  int w_i = 0;                /* Window index */
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

    /* Post a batch of RECVs */
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
