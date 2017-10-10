#include "hrd.h"
#include "main.h"

#define DGRAM_BUF_SIZE 4096

void* run_client(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this client thread */

  int ib_port_index = params.dual_port == 0 ? 0 : clt_gid % 2;

  /*
   * TODO: The client creates a connected buffer because the libhrd API
   * requires a buffer when creating connected queue pairs. This should be
   * fixed in the API.
   */
  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_gid,                /* local_hid */
      ib_port_index, -1,      /* port_index, numa_node_id */
      1, USE_UC,              /* conn qps, uc */
      NULL, 4096, -1,         /* prealloc buf, buf size, key */
      1, DGRAM_BUF_SIZE, -1); /* num_dgram_qps, dgram_buf_size, key */

  /* Buffers to send requests from - one buffer for each posting */
  long long* req_buf[MAX_POSTLIST];
  for (i = 0; i < MAX_POSTLIST; i++) {
    req_buf[i] = malloc(params.size);
    assert(req_buf[i] != NULL);
    memset(req_buf[i], 1, params.size);
  }

  char mstr_qp_name[HRD_QP_NAME_SIZE];
  sprintf(mstr_qp_name, "master-%d", clt_gid);
  char clt_conn_qp_name[HRD_QP_NAME_SIZE];
  sprintf(clt_conn_qp_name, "client-conn-%d", clt_gid);
  char clt_dgram_qp_name[HRD_QP_NAME_SIZE];
  sprintf(clt_dgram_qp_name, "client-dgram-%d", clt_gid);

  hrd_publish_conn_qp(cb, 0, clt_conn_qp_name);
  hrd_publish_dgram_qp(cb, 0, clt_dgram_qp_name);
  printf("main: Client %s published conn and dgram. Waiting for master %s\n",
         clt_conn_qp_name, mstr_qp_name);

  struct hrd_qp_attr* mstr_qp = NULL;
  while (mstr_qp == NULL) {
    mstr_qp = hrd_get_published_qp(mstr_qp_name);
    if (mstr_qp == NULL) {
      usleep(200000);
    }
  }

  printf("main: Client %s found master! Connecting..\n", clt_conn_qp_name);
  hrd_connect_qp(cb, 0, mstr_qp);
  hrd_wait_till_ready(mstr_qp_name);

  int ret;
  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_sge sgl[MAX_POSTLIST];
  struct ibv_wc wc;
  long long last_req[NUM_WORKERS] = {0};
  long long rolling_iter = 0; /* For throughput measurement */
  long long nb_tx = 0;        /* Total requests performed */
  int wn = 0;                 /* Worker number */

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

    for (i = 0; i < params.postlist; i++) {
      /* Poll for 1 response and post a new RECV */
      if (nb_tx >= 512) {
        hrd_poll_cq(cb->dgram_recv_cq[0], 1, &wc);
      }

      hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf, DGRAM_BUF_SIZE,
                          cb->dgram_buf_mr->lkey);

      sgl[i].addr = (uint64_t)(uintptr_t)req_buf[i];
      sgl[i].length = params.size;

      wr[i].opcode = IBV_WR_RDMA_WRITE;
      wr[i].num_sge = 1;
      wr[i].next = (i == params.postlist - 1) ? NULL : &wr[i + 1];
      wr[i].sg_list = &sgl[i];

      wr[i].send_flags = (nb_tx & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx & UNSIG_BATCH_) == UNSIG_BATCH_) {
        hrd_poll_cq(cb->conn_cq[0], 1, &wc);
      }

      wr[i].send_flags |= IBV_SEND_INLINE;

      last_req[wn]++; /* >=1 for server to detect request */
      req_buf[i][0] = last_req[wn];

      wr[i].wr.rdma.remote_addr = mstr_qp->buf_addr +
                                  (wn * NUM_CLIENTS * CACHELINE_SIZE) +
                                  (clt_gid * CACHELINE_SIZE);
      wr[i].wr.rdma.rkey = mstr_qp->rkey;

      /* Compute the next worker to send a request to */
      HRD_MOD_ADD(wn, NUM_WORKERS);

      rolling_iter++;
      nb_tx++;
    }

    ret = ibv_post_send(cb->conn_qp[0], wr, &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);
  }

  return NULL;
}
