#include "hrd.h"
#include "main.h"

#define DGRAM_BUF_SIZE 4096

void* run_client(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this client thread */
  int num_client_ports = params.num_client_ports;
  int num_server_ports = params.num_server_ports;

  /*
   * Ensure that all responses fit in dgram_buf. We post RECVs of size
   * CACHELINE_SIZE to ensure that the 8-byte counter and GRH fit.
   */
  assert(WINDOW_SIZE * CACHELINE_SIZE <= DGRAM_BUF_SIZE);

  /* This is the only port used by this client */
  int ib_port_index = params.base_port_index + clt_gid % num_client_ports;

  /*
   * The virtual server port index to connect to. This index is relative to
   * the server's base_port_index (that the client does not know).
   */
  int srv_virt_port_index = clt_gid % num_server_ports;

  /*
   * TODO: The client creates a connected buffer because the libhrd API
   * requires a buffer when creating connected queue pairs. This should be
   * fixed in the API.
   */
  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_gid,                /* local_hid */
      ib_port_index, -1,      /* port_index, numa_node_id */
      1, 1,                   /* #conn qps, uc */
      NULL, 4096, -1,         /* prealloc conn buf, buf size, key */
      1, DGRAM_BUF_SIZE, -1); /* num_dgram_qps, dgram_buf_size, key */

  char mstr_qp_name[HRD_QP_NAME_SIZE];
  sprintf(mstr_qp_name, "master-%d-%d", srv_virt_port_index, clt_gid);

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

  /* Start the real work */
  int ret;
  long long req_buf = 1; /* Any non-zero number will do */

  /* Some tracking info */
  int ws[NUM_WORKERS] = {0}; /* Window slot to use for a worker */

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc[WINDOW_SIZE];

  struct ibv_recv_wr recv_wr[WINDOW_SIZE], *bad_recv_wr;
  struct ibv_sge recv_sgl[WINDOW_SIZE];

  long long rolling_iter = 0; /* For throughput measurement */
  long long nb_tx = 0;        /* Total requests performed or queued */
  int wn = 0;                 /* Worker number */

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  /* Fill the RECV queue */
  for (i = 0; i < WINDOW_SIZE; i++) {
    hrd_post_dgram_recv(
        cb->dgram_qp[0],
        (void*)&cb->dgram_buf[i * CACHELINE_SIZE], /* i < WINDOW_SIZE */
        CACHELINE_SIZE, cb->dgram_buf_mr->lkey);
  }

  while (1) {
    if (rolling_iter >= K_512) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Client %d: %.2f IOPS. nb_tx = %lld\n", clt_gid,
             K_512 / seconds, nb_tx);

      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /* Re-fill depleted RECVs */
    if (nb_tx % WINDOW_SIZE == 0 && nb_tx > 0) {
      for (i = 0; i < WINDOW_SIZE; i++) {
        recv_sgl[i].length = DGRAM_BUF_SIZE;
        recv_sgl[i].lkey = cb->dgram_buf_mr->lkey;
        recv_sgl[i].addr = (uintptr_t)&cb->dgram_buf[i * CACHELINE_SIZE];

        recv_wr[i].sg_list = &recv_sgl[i];
        recv_wr[i].num_sge = 1;
        recv_wr[i].next = (i == WINDOW_SIZE - 1) ? NULL : &recv_wr[i + 1];
      }

      ret = ibv_post_recv(cb->dgram_qp[0], &recv_wr[0], &bad_recv_wr);
      CPE(ret, "ibv_post_recv error", ret);
    }

    if (nb_tx % WINDOW_SIZE == 0 && nb_tx > 0) {
      hrd_poll_cq(cb->dgram_recv_cq[0], WINDOW_SIZE, wc);
    }

    wn = nb_tx % NUM_WORKERS; /* Choose a worker */

    /* Forge the RDMA work request */
    sgl.length = sizeof(long long);
    sgl.addr = (uint64_t)(uintptr_t)&req_buf;

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = NULL;
    wr.sg_list = &sgl;

    wr.send_flags = (nb_tx & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
    if ((nb_tx & UNSIG_BATCH_) == UNSIG_BATCH_) {
      hrd_poll_cq(cb->conn_cq[0], 1, wc);
    }
    wr.send_flags |= IBV_SEND_INLINE;

    wr.wr.rdma.remote_addr =
        mstr_qp->buf_addr + OFFSET(wn, clt_gid, ws[wn]) * sizeof(long long);
    wr.wr.rdma.rkey = mstr_qp->rkey;

    ret = ibv_post_send(cb->conn_qp[0], &wr, &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);
    // printf("Client %d: sending request index %lld\n", clt_gid, nb_tx);

    rolling_iter++;
    nb_tx++;
    HRD_MOD_ADD(ws[wn], WINDOW_SIZE);
  }

  return NULL;
}
