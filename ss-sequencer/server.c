#include "hrd.h"
#include "main.h"

void* run_server(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id; /* Global ID of this server thread */
  int clt_gid = srv_gid;
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      srv_gid,                   /* local_hid */
      ib_port_index, -1,         /* port_index, numa_node_id */
      0, 0,                      /* conn qps, use uc */
      NULL, 0, -1,               /* prealloc conn buf, conn buf size, key */
      NUM_UD_QPS, BUF_SIZE, -1); /* num_dgram_qps, dgram_buf_size, key */

  /* Buffer to receive requests into */
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  /* Buffer to send response counters from */
  uint64_t resp_buf[MAX_POSTLIST];
  memset(resp_buf, 0, MAX_POSTLIST * sizeof(uint64_t));

  char srv_name[HRD_QP_NAME_SIZE];
  sprintf(srv_name, "server-%d", srv_gid);
  char clt_name[HRD_QP_NAME_SIZE];
  sprintf(clt_name, "client-%d", clt_gid);

  for (i = 0; i < HRD_Q_DEPTH; i++) {
    /*
     * The clients use header-only SENDs, so the NIC won't really DMA data
     * to dgram_buf. Use a valid address anyway.
     */
    hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf, CACHELINE_SIZE,
                        cb->dgram_buf_mr->lkey);
  }

  hrd_publish_dgram_qp(cb, 0, srv_name);
  printf("server: Server %s published. Now polling..\n", srv_name);

  /* We'll initialize address handles on demand */
  struct ibv_ah* ah[HRD_MAX_LID];
  memset(ah, 0, HRD_MAX_LID * sizeof(uintptr_t));

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_recv_wr recv_wr[MAX_POSTLIST], *bad_recv_wr;
  struct ibv_wc wc[MAX_POSTLIST];
  struct ibv_sge sgl[MAX_POSTLIST];
  long long rolling_iter = 0; /* For throughput measurement */
  long long nb_tx[NUM_UD_QPS] = {0};
  long long nb_tx_tot = 0, nb_post_send = 0;
  int w_i = 0;  /* Window index */
  int qp_i = 0; /* Queue pair to use */
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= M_4) {
      clock_gettime(CLOCK_REALTIME, &end);

      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      params.tput[srv_gid] = M_4 / seconds;
      printf("main: Server %d: %.2f IOPS. Average postlist = %.2f\n", srv_gid,
             params.tput[srv_gid], (double)nb_tx_tot / nb_post_send);

      if (srv_gid == 0) {
        double total_tput = 0;
        for (i = 0; i < NUM_SERVER_THREADS; i++) {
          total_tput += params.tput[i];
        }
        hrd_red_printf("main: Total = %.2f. Counter = %lld\n", total_tput,
                       (long long)*(params.counter));
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &start);
    }

    int num_comps = ibv_poll_cq(cb->dgram_recv_cq[0], params.postlist, wc);
    if (num_comps == 0) {
      continue;
    }

    uint64_t base = __sync_fetch_and_add(params.counter, num_comps);

    /* Post a batch of RECVs */
    for (w_i = 0; w_i < num_comps; w_i++) {
      sgl[w_i].length = CACHELINE_SIZE;
      sgl[w_i].lkey = cb->dgram_buf_mr->lkey;
      sgl[w_i].addr = (uintptr_t)&cb->dgram_buf[0];

      recv_wr[w_i].sg_list = &sgl[w_i];
      recv_wr[w_i].num_sge = 1;
      recv_wr[w_i].next = (w_i == num_comps - 1) ? NULL : &recv_wr[w_i + 1];
    }
    ret = ibv_post_recv(cb->dgram_qp[0], &recv_wr[0], &bad_recv_wr);
    CPE(ret, "ibv_post_recv error", ret);

    for (w_i = 0; w_i < num_comps; w_i++) {
      int s_lid = wc[w_i].slid; /* Src LID for this request */
      if (unlikely(ah[s_lid] == NULL)) {
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

      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == num_comps - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];
      sgl[w_i].addr = (uint64_t)(uintptr_t)&resp_buf[w_i];

      uint64_t resp_val = base + w_i;

      /*
       * Check if client's assumed value of the 32 MSBs is correct. If
       * not, send the entire counter.
       */
      if (wc[w_i].imm_data == (uint32_t)(resp_val >> 32)) {
        wr[w_i].opcode = IBV_WR_SEND_WITH_IMM;
        wr[w_i].imm_data = (uint32_t)(resp_val & 0xffffffffU);
        sgl[w_i].length = 0;
      } else {
        wr[w_i].opcode = IBV_WR_SEND;
        resp_buf[w_i] = resp_val;
        sgl[w_i].length = sizeof(long long);
      }

      /*
       * UNSIG_BATCH >= 2 * postlist ensures that we poll for a
       * completed send() only after we have performed a signaled send()
       */
      wr[w_i].send_flags =
          ((nb_tx[qp_i] & UNSIG_BATCH_) == 0) ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx[qp_i] & UNSIG_BATCH_) == UNSIG_BATCH_) {
        hrd_poll_cq(cb->dgram_send_cq[qp_i], 1, wc);
      }

      wr[w_i].send_flags |= IBV_SEND_INLINE;

#if DISABLE_SERVER_POSTLIST == 1
      wr[w_i].next = NULL;
      ret = ibv_post_send(cb->dgram_qp[qp_i], &wr[w_i], &bad_send_wr);
      CPE(ret, "ibv_post_send error", ret);
      nb_post_send++;
#endif

      nb_tx[qp_i]++;
      nb_tx_tot++;
      rolling_iter++;
    }

#if DISABLE_SERVER_POSTLIST == 0
    ret = ibv_post_send(cb->dgram_qp[qp_i], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);
    nb_post_send++;
#endif

    HRD_MOD_ADD(qp_i, NUM_UD_QPS);
  }

  return NULL;
}
