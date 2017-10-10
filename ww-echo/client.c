#include "hrd.h"
#include "main.h"

void* run_client(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this client thread */
  int srv_gid = clt_gid;   /* One-to-one connections */
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_gid,            /* local_hid */
      ib_port_index, -1,  /* port_index, numa_node_id */
      1, params.use_uc,   /* conn qps, uc */
      NULL, BUF_SIZE, -1, /* prealloc buf, buf size, shm key */
      0, 0, -1);          /* #qps, uc, buf size, shm key*/

  /* Buffer to receive responses into */
  memset((void*)cb->conn_buf, 0, BUF_SIZE);

  /* Buffer to send requests from */
  long long* req_buf = malloc(params.size);
  assert(req_buf != 0);
  memset(req_buf, 1, params.size);

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

  struct ibv_send_wr wr[MAX_WINDOW_SIZE], *bad_send_wr;
  struct ibv_sge sgl[MAX_WINDOW_SIZE];
  struct ibv_wc wc;
  long long rolling_iter = 0; /* For throughput measurement */
  int w_i = 0;                /* Window index */
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  /*
   * The reads/writes at different window positions should be done to/from
   * different cache lines.
   */
  int offset = CACHELINE_SIZE;
  while (offset < params.size) {
    offset += CACHELINE_SIZE;
  }
  assert(offset * params.window <= BUF_SIZE);

  while (1) {
    if (rolling_iter >= M_4) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Client %d: %.2f Mops\n", clt_gid, M_4 / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (w_i = 0; w_i < params.window; w_i++) {
      wr[w_i].opcode = IBV_WR_RDMA_WRITE;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == params.window - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = (w_i == 0) ? IBV_SEND_SIGNALED : 0;
      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = (uint64_t)(uintptr_t)req_buf;
      sgl[w_i].length = params.size;

      wr[w_i].wr.rdma.remote_addr = srv_qp->buf_addr + (offset * w_i);
      wr[w_i].wr.rdma.rkey = srv_qp->rkey;

      rolling_iter++;
    }

    ret = ibv_post_send(cb->conn_qp[0], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    hrd_poll_cq(cb->conn_cq[0], 1, &wc); /* Poll for w_i = 0 */

    /* Wait for some responses */
    while (cb->conn_buf[0] == 0) {
      /* Do nothing */
    }
    cb->conn_buf[0] = 0;
  }

  return NULL;
}
