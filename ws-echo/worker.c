#include "hrd.h"
#include "main.h"

#define NUM_UD_QPS 3 /* Number of UD QPs per port */

void* run_worker(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;
  int wrkr_lid = params.id; /* Local ID of this worker thread*/

  struct hrd_ctrl_blk* cb[2]; /* 1 control block per port */
  volatile LL* req_buf[2];    /* 1 request region per port */

  for (i = 0; i < 2; i++) {
    cb[i] = hrd_ctrl_blk_init(
        wrkr_lid,                                /* local_hid */
        (params.dual_port == 0 ? 0 : i % 2), -1, /* port index, numa node */
        0, 0,                                    /* conn qps, uc */
        NULL, 0, -1,           /* prealloc conn buf, buf size, key */
        NUM_UD_QPS, 4096, -1); /* num_dgram_qps, dgram_buf_size, key */

    /* Map the reqeust regions created by the master */
    int sid = shmget(MASTER_SHM_KEY + i, RR_SIZE, SHM_HUGETLB | 0666);
    assert(sid != -1);
    req_buf[i] = shmat(sid, 0, 0);
    assert(req_buf[i] != (void*)-1);
  }

  uint8_t* resp_buf = malloc(params.size);
  memset(resp_buf, (uint8_t)wrkr_lid, params.size);

  /* Create an address handle for each client */
  struct ibv_ah* ah[NUM_CLIENTS];
  memset(ah, 0, NUM_CLIENTS * sizeof(uintptr_t));
  struct hrd_qp_attr* clt_qp[NUM_CLIENTS];

  for (i = 0; i < NUM_CLIENTS; i++) {
    char clt_name[HRD_QP_NAME_SIZE];
    sprintf(clt_name, "client-dgram-%d", i);

    /* Get the UD queue pair for the ith client */
    clt_qp[i] = NULL;
    while (clt_qp[i] == NULL) {
      clt_qp[i] = hrd_get_published_qp(clt_name);
      if (clt_qp[i] == NULL) {
        usleep(200000);
      }
    }

    printf("main: Worker %d found client %s\n", wrkr_lid, clt_name);

    struct ibv_ah_attr ah_attr = {
        .is_global = 0,
        .dlid = clt_qp[i]->lid,
        .sl = 0,
        .src_path_bits = 0,
        /* port_num (> 1): local physical port from which pkts are sent */
        .port_num = params.dual_port == 0 ? 1 : (i % 2) + 1,
    };

    ah[i] = ibv_create_ah(cb[i % 2]->pd, &ah_attr);
    assert(ah[i] != NULL);
  }

  int ret;
  struct ibv_send_wr wr[NUM_CLIENTS], *bad_send_wr = NULL;

  /* Per-port start and end postlist pointers */
  struct ibv_send_wr *first_send_wr[2] = {NULL}, *last_send_wr[2] = {NULL};
  struct ibv_sge sgl[NUM_CLIENTS];
  struct ibv_wc wc;
  long long last_req[NUM_CLIENTS] = {0};
  long long rolling_iter = 0;             /* For throughput measurement */
  long long nb_tx[2][NUM_UD_QPS] = {{0}}; /* For per-port UD QP polling */
  int ud_qp_i = 0;                        /* UD QP index: same for both ports */
  long long nb_tx_tot = 0;
  long long nb_post_send = 0;

  /*
   * Index of the client to poll. This is required to start the next postlist
   * from the next un-polled client.
   */
  int clt_i = -1;

  /*
   * We poll sequentially from client 0 -- NUM_CLIENTS - 1. port_i is the
   * port to use for the current client's response.
   */
  int port_i;

  /* The request region segment start address for this worker */
  int req_buf_base = wrkr_lid * NUM_CLIENTS * SLOT_SIZE_LL;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= M_4) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Worker %d: %.2f Mops. Average postlist = %.2f\n", wrkr_lid,
             M_4 / seconds, (double)nb_tx_tot / nb_post_send);

      rolling_iter = 0;
      nb_tx_tot = 0;
      nb_post_send = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /* Do a pass over requests from all clients */
    int nb_new_req[2] = {0};
    for (i = 0; i < NUM_CLIENTS; i++) {
      /* Start where we left off last time */
      HRD_MOD_ADD(clt_i, NUM_CLIENTS);
      port_i = clt_i & 1;

      if (req_buf[port_i][req_buf_base + (clt_i * SLOT_SIZE_LL)] ==
          last_req[clt_i]) {
        continue;
      }

      /* Valid request from client i. Yay! */
      last_req[clt_i]++;

      /* Add the SEND response for this client to the postlist */
      if (nb_new_req[port_i] == 0) {
        first_send_wr[port_i] = &wr[i];
        last_send_wr[port_i] = &wr[i];
      } else {
        last_send_wr[port_i]->next = &wr[i];
        last_send_wr[port_i] = &wr[i];
      }

      /* Fill in the work request */
      wr[i].wr.ud.ah = ah[clt_i];
      wr[i].wr.ud.remote_qpn = clt_qp[clt_i]->qpn;
      wr[i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

      wr[i].opcode = IBV_WR_SEND;
      wr[i].num_sge = 1;
      wr[i].sg_list = &sgl[i];

      /*
       * UNSIG_BATCH >= 2 * MAX_POSTLIST ensures that we poll for a
       * completed SEND only after we have performed a signaled SEND.
       */
      wr[i].send_flags = ((nb_tx[port_i][ud_qp_i] & UNSIG_BATCH_) == 0)
                             ? IBV_SEND_SIGNALED
                             : 0;
      if ((nb_tx[port_i][ud_qp_i] & UNSIG_BATCH_) == UNSIG_BATCH_) {
        hrd_poll_cq(cb[port_i]->dgram_send_cq[ud_qp_i], 1, &wc);
      }

      wr[i].send_flags |= IBV_SEND_INLINE;

      sgl[i].addr = (uint64_t)(uintptr_t)resp_buf;
      sgl[i].length = params.size;

      rolling_iter++;
      nb_tx[port_i][ud_qp_i]++; /* Must increment inside loop */
      nb_tx_tot++;

      nb_new_req[port_i]++;
      if (nb_new_req[0] + nb_new_req[1] >= 2 * params.postlist) {
        break;
      }
    }

    for (i = 0; i < 2; i++) {
      if (nb_new_req[i] != 0) {
        last_send_wr[i]->next = NULL;
        ret = ibv_post_send(cb[i]->dgram_qp[ud_qp_i], first_send_wr[i],
                            &bad_send_wr);
        CPE(ret, "ibv_post_send error", ret);
        nb_post_send++;
      }
    }

    /* Use a different UD QP for the next postlist */
    HRD_MOD_ADD(ud_qp_i, NUM_UD_QPS);

    hrd_nano_sleep(300);
  }

  return NULL;
}
