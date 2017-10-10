#include <byteswap.h>
#include "hrd.h"
#include "main.h"

#define MEASURE_LATENCY 1
#define PRINT_COUNTER 0 /* Print received sequencer result */

void* run_client(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;

  // Sanity checks

  /* Ensure that each server thread has enough RECVs */
  int max_clients_per_server_thread =
      (MAX_NUM_CLIENTS + NUM_SERVER_THREADS - 1) / NUM_SERVER_THREADS;
  assert(max_clients_per_server_thread * params.postlist < HRD_Q_DEPTH);

  assert(params.id < MAX_NUM_CLIENTS);
  assert(params.postlist * CACHELINE_SIZE < BUF_SIZE);

  int clt_gid = params.id; /* Global ID of this client thread */
  int clt_local_hid = clt_gid % params.num_threads;

  int srv_gid = clt_gid % NUM_SERVER_THREADS;
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_local_hid, ib_port_index, -1, /* port_index, numa_node_id */
      0, 0,                             /* conn qps, use uc */
      NULL, 0, -1,      /* prealloc conn buf, conn buf size, key */
      1, BUF_SIZE, -1); /* num_dgram_qps, dgram_buf_size, key */

  /* Buffer to receive responses into */
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

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
  long long rolling_iter = 0; /* For throughput measurement */
  long long nb_tx = 0;        /* For selective signaling */
  long long tot_non_zero = 0;
  int req_wi = 0, resp_wi = 0; /* Request and response window index */
  int reqs_to_send = params.postlist;
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  struct timespec lat_start[MAX_POSTLIST], lat_end[MAX_POSTLIST];
  long long lat_us_tot = 0;
  _unused(lat_start);
  _unused(lat_end);
  _unused(lat_us_tot);

  uint64_t counter_hi = 0; /* Assumed higher 32 bits of 64B counter */
  uint64_t resp_counter;

  while (1) {
    if (rolling_iter >= K_512) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf(
          "main: Client %d: %.2f IOPS. Average latency = %.2f us. "
          "Total non zero SENDs received = %lld\n",
          clt_gid, rolling_iter / seconds, (double)lat_us_tot / rolling_iter,
          tot_non_zero);

      rolling_iter = 0;
      lat_us_tot = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    assert(reqs_to_send > 0);

    int i;
    for (i = 0; i < reqs_to_send; i++) {
#if MEASURE_LATENCY == 1
      clock_gettime(CLOCK_REALTIME, &lat_start[req_wi]);
#endif
      /*
       * Post a recv to a separate cacheline for each postlist element.
       * For some elements of the postlist, the server might use a
       * non-immediate SEND.
       */
      hrd_post_dgram_recv(cb->dgram_qp[0],
                          (void*)&cb->dgram_buf[req_wi * CACHELINE_SIZE],
                          40 + sizeof(long long), cb->dgram_buf_mr->lkey);

      wr[i].wr.ud.ah = ah;
      wr[i].wr.ud.remote_qpn = srv_qp->qpn;
      wr[i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

      wr[i].opcode = IBV_WR_SEND_WITH_IMM;
      wr[i].num_sge = 1;
      wr[i].next = (i == reqs_to_send - 1) ? NULL : &wr[i + 1];
      wr[i].sg_list = &sgl[i];

      /* Send the expected MSBs to the server */
      wr[i].imm_data = (uint32_t)(counter_hi >> 32);

      wr[i].send_flags = (nb_tx & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx & UNSIG_BATCH_) == UNSIG_BATCH_) {
        hrd_poll_cq(cb->dgram_send_cq[0], 1, wc);
      }

      wr[i].send_flags |= IBV_SEND_INLINE;

      sgl[i].length = 0; /* No need to assign sgl[i].addr */

      HRD_MOD_ADD(req_wi, params.postlist);
      nb_tx++;
      rolling_iter++;
    }

    ret = ibv_post_send(cb->dgram_qp[0], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    reqs_to_send = 0;
    int num_resps = 0;
    while (num_resps == 0) {
      num_resps = ibv_poll_cq(cb->dgram_recv_cq[0], params.postlist, wc);
    }

    reqs_to_send = num_resps;

    for (i = 0; i < num_resps; i++) {
#if MEASURE_LATENCY == 1
      /*
       * Responses to requests may be received in a different order than
       * requests if the server thread uses multiple QPs (which don't
       * really help this sequencer). However, this is a sequencer, so
       * we don't care about out-of-order responses: any response can
       * be assumed as the response for any request.
       */
      clock_gettime(CLOCK_REALTIME, &lat_end[resp_wi]);

      lat_us_tot +=
          (lat_end[resp_wi].tv_sec - lat_start[resp_wi].tv_sec) * 1000000 +
          (lat_end[resp_wi].tv_nsec - lat_start[resp_wi].tv_nsec) / 1000.0;

      HRD_MOD_ADD(resp_wi, params.postlist);
#endif
      /*
       * If the server did not use SEND_WITH_IMM, the complete counter
       * value is in the request buffer.
       */
      if ((wc[i].wc_flags & IBV_WC_WITH_IMM) == 0) {
        tot_non_zero++;
        int buf_offset = i * CACHELINE_SIZE + 40;
        resp_counter = *((uint64_t*)&cb->dgram_buf[buf_offset]);
        counter_hi = ((resp_counter >> 32) << 32);
      } else {
        resp_counter = counter_hi + wc[i].imm_data;
      }

      if (PRINT_COUNTER == 1) {
        printf("main: Client %d got counter = %lld\n", clt_gid,
               (long long)resp_counter);
      }
    }
  }

  return NULL;
}
