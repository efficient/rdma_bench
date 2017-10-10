#include "hrd.h"

/*
 * If @prealloc_conn_buf != NULL, @conn_buf_size is the size of the preallocated
 * buffer. If @prealloc_conn_buf == NULL, @conn_buf_size is the size of the
 * new buffer to create.
 */
struct hrd_ctrl_blk* hrd_ctrl_blk_init(
    int local_hid, int port_index,
    int numa_node_id, /* -1 means don't use hugepages */
    int num_conn_qps, int use_uc, volatile void* prealloc_conn_buf,
    int conn_buf_size, int conn_buf_shm_key, int num_dgram_qps,
    int dgram_buf_size, int dgram_buf_shm_key) {
#if HRD_CONNECT_IB_ATOMICS == 1
  hrd_red_printf(
      "HRD: Connect-IB atomics enabled. This QP setup has not "
      "been tested for non-atomics performance.\n");
  sleep(1);
#endif

  hrd_red_printf(
      "HRD: creating control block %d: port %d, socket %d, "
      "conn qps %d, UC %d, conn buf %d bytes (key %d), "
      "dgram qps %d, dgram buf %d bytes (key %d)\n",
      local_hid, port_index, numa_node_id, num_conn_qps, use_uc, conn_buf_size,
      conn_buf_shm_key, num_dgram_qps, dgram_buf_size, dgram_buf_shm_key);

  /*
   * Check arguments for sanity.
   * @local_hid can be anything: it's just control block identifier that is
   * useful in printing debug info.
   */
  assert(port_index >= 0 && port_index <= 16);
  assert(numa_node_id >= -1 && numa_node_id <= 8);
  assert(num_conn_qps >= 0);
  assert(use_uc == 0 || use_uc == 1);
  assert(conn_buf_size >= 0 && conn_buf_size <= M_1024);

  /* If there is no preallocated buffer, shm key can be =/>/< 0 */
  if (prealloc_conn_buf != NULL) {
    assert(conn_buf_shm_key == -1);
  }
  assert(num_dgram_qps >= 0 && num_dgram_qps <= M_2);
  assert(dgram_buf_size >= 0 && dgram_buf_size <= M_1024);

  if (num_conn_qps == 0 && num_dgram_qps == 0) {
    hrd_red_printf(
        "HRD: Control block initialization without QPs. Are you"
        " sure you want to do this?\n");
    assert(false);
  }

  struct hrd_ctrl_blk* cb =
      (struct hrd_ctrl_blk*)malloc(sizeof(struct hrd_ctrl_blk));
  memset(cb, 0, sizeof(struct hrd_ctrl_blk));

  /* Fill in the control block */
  cb->local_hid = local_hid;
  cb->port_index = port_index;
  cb->numa_node_id = numa_node_id;
  cb->use_uc = use_uc;

  cb->num_conn_qps = num_conn_qps;
  cb->conn_buf_size = conn_buf_size;
  cb->conn_buf_shm_key = conn_buf_shm_key;

  cb->num_dgram_qps = num_dgram_qps;
  cb->dgram_buf_size = dgram_buf_size;
  cb->dgram_buf_shm_key = dgram_buf_shm_key;

  /* Get the device to use. This fills in cb->device_id and cb->dev_port_id */
  struct ibv_device* ib_dev = hrd_resolve_port_index(cb, port_index);
  CPE(!ib_dev, "HRD: IB device not found", 0);

  /* Use a single device context and PD for all QPs */
  cb->ctx = ibv_open_device(ib_dev);
  CPE(!cb->ctx, "HRD: Couldn't get context", 0);

  cb->pd = ibv_alloc_pd(cb->ctx);
  CPE(!cb->pd, "HRD: Couldn't allocate PD", 0);

  int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

  /*
   * Create datagram QPs and transition them RTS.
   * Create and register datagram RDMA buffer.
   */
  if (num_dgram_qps >= 1) {
    cb->dgram_qp =
        (struct ibv_qp**)malloc(num_dgram_qps * sizeof(struct ibv_qp*));
    cb->dgram_send_cq =
        (struct ibv_cq**)malloc(num_dgram_qps * sizeof(struct ibv_cq*));
    cb->dgram_recv_cq =
        (struct ibv_cq**)malloc(num_dgram_qps * sizeof(struct ibv_cq*));

    assert(cb->dgram_qp != NULL && cb->dgram_send_cq != NULL &&
           cb->dgram_recv_cq != NULL);

    hrd_create_dgram_qps(cb);

    /* Create and register dgram_buf */
    int reg_size = 0;

    if (numa_node_id >= 0) {
      /* Hugepages */
      while (reg_size < dgram_buf_size) {
        reg_size += M_2;
      }

      /* SHM key 0 is hard to free later */
      assert(dgram_buf_shm_key >= 1 && dgram_buf_shm_key <= 128);
      cb->dgram_buf = (volatile uint8_t*)hrd_malloc_socket(
          dgram_buf_shm_key, reg_size, numa_node_id);
    } else {
      reg_size = dgram_buf_size;
      cb->dgram_buf = (volatile uint8_t*)memalign(4096, reg_size);
    }

    assert(cb->dgram_buf != NULL);
    memset((char*)cb->dgram_buf, 0, reg_size);

    cb->dgram_buf_mr =
        ibv_reg_mr(cb->pd, (char*)cb->dgram_buf, reg_size, ib_flags);
    assert(cb->dgram_buf_mr != NULL);
  }

  /*
   * Create connected QPs and transition them to RTS.
   * Create and register connected QP RDMA buffer.
   */
  if (num_conn_qps >= 1) {
    cb->conn_qp =
        (struct ibv_qp**)malloc(num_conn_qps * sizeof(struct ibv_qp*));
    cb->conn_cq =
        (struct ibv_cq**)malloc(num_conn_qps * sizeof(struct ibv_cq*));

    assert(cb->conn_qp != NULL && cb->conn_cq != NULL);

    hrd_create_conn_qps(cb);

    if (prealloc_conn_buf == NULL) {
      /* Create and register conn_buf - always make it multiple of 2 MB*/
      int reg_size = 0;

      /* If numa_node_id < 0, use standard heap memory */
      if (numa_node_id >= 0) {
        /* Hugepages */
        while (reg_size < conn_buf_size) {
          reg_size += M_2;
        }

        /* SHM key 0 is hard to free later */
        assert(conn_buf_shm_key >= 1 && conn_buf_shm_key <= 128);
        cb->conn_buf = (volatile uint8_t*)hrd_malloc_socket(
            conn_buf_shm_key, reg_size, numa_node_id);
      } else {
        reg_size = conn_buf_size;
        cb->conn_buf = (volatile uint8_t*)memalign(4096, reg_size);
        assert(cb->conn_buf != NULL);
      }
      memset((char*)cb->conn_buf, 0, reg_size);
      cb->conn_buf_mr =
          ibv_reg_mr(cb->pd, (char*)cb->conn_buf, reg_size, ib_flags);
      assert(cb->conn_buf_mr != NULL);
    } else {
      cb->conn_buf = (volatile uint8_t*)prealloc_conn_buf;
      cb->conn_buf_mr =
          ibv_reg_mr(cb->pd, (char*)cb->conn_buf, cb->conn_buf_size, ib_flags);
      assert(cb->conn_buf_mr != NULL);
    }
  }

  /* Create an array in cb for holding work completions */
  cb->wc = (struct ibv_wc*)malloc(HRD_Q_DEPTH * sizeof(struct ibv_wc));
  assert(cb->wc != NULL);
  memset(cb->wc, 0, HRD_Q_DEPTH * sizeof(struct ibv_wc));

  return cb;
}

/* Free up the resources taken by @cb. Return -1 if something fails, else 0. */
int hrd_ctrl_blk_destroy(struct hrd_ctrl_blk* cb) {
  int i;
  hrd_red_printf("HRD: Destroying control block %d\n", cb->local_hid);

  /* Destroy QPs and CQs. QPs must be destroyed before CQs. */
  for (i = 0; i < cb->num_dgram_qps; i++) {
    assert(cb->dgram_send_cq[i] != NULL && cb->dgram_recv_cq[i] != NULL);
    assert(cb->dgram_qp[i] != NULL);

    if (ibv_destroy_qp(cb->dgram_qp[i])) {
      fprintf(stderr, "HRD: Couldn't destroy dgram QP %d\n", i);
      return -1;
    }

    if (ibv_destroy_cq(cb->dgram_send_cq[i])) {
      fprintf(stderr, "HRD: Couldn't destroy dgram SEND CQ %d\n", i);
      return -1;
    }

    if (ibv_destroy_cq(cb->dgram_recv_cq[i])) {
      fprintf(stderr, "HRD: Couldn't destroy dgram RECV CQ %d\n", i);
      return -1;
    }
  }

  for (i = 0; i < cb->num_conn_qps; i++) {
    assert(cb->conn_cq[i] != NULL && cb->conn_qp[i] != NULL);

    if (ibv_destroy_qp(cb->conn_qp[i])) {
      fprintf(stderr, "HRD: Couldn't destroy conn QP %d\n", i);
      return -1;
    }

    if (ibv_destroy_cq(cb->conn_cq[i])) {
      fprintf(stderr, "HRD: Couldn't destroy conn CQ %d\n", i);
      return -1;
    }
  }

  /* Destroy memory regions */
  if (cb->num_dgram_qps > 0) {
    assert(cb->dgram_buf_mr != NULL && cb->dgram_buf != NULL);
    if (ibv_dereg_mr(cb->dgram_buf_mr)) {
      fprintf(stderr, "HRD: Couldn't deregister dgram MR for cb %d\n",
              cb->local_hid);
      return -1;
    }

    if (cb->numa_node_id >= 0) {
      if (hrd_free(cb->dgram_buf_shm_key, (void*)cb->dgram_buf)) {
        fprintf(stderr, "HRD: Error freeing dgram hugepages for cb %d\n",
                cb->local_hid);
      }
    } else {
      free((void*)cb->dgram_buf);
    }
  }

  if (cb->num_conn_qps > 0) {
    assert(cb->conn_buf_mr != NULL);
    if (ibv_dereg_mr(cb->conn_buf_mr)) {
      fprintf(stderr, "HRD: Couldn't deregister conn MR for cb %d\n",
              cb->local_hid);
      return -1;
    }

    if (cb->numa_node_id >= 0) {
      if (hrd_free(cb->conn_buf_shm_key, (void*)cb->conn_buf)) {
        fprintf(stderr, "HRD: Error freeing conn hugepages for cb %d\n",
                cb->local_hid);
      }
    } else {
      free((void*)cb->conn_buf);
    }
  }

  /* Destroy protection domain */
  if (ibv_dealloc_pd(cb->pd)) {
    fprintf(stderr, "HRD: Couldn't dealloc PD for cb %d\n", cb->local_hid);
    return -1;
  }

  /* Destroy device context */
  if (ibv_close_device(cb->ctx)) {
    fprintf(stderr, "Couldn't release context for cb %d\n", cb->local_hid);
    return -1;
  }

  hrd_red_printf("HRD: Control block %d destroyed.\n", cb->local_hid);
  return 0;
}

/* Create datagram QPs and transition them to RTS */
void hrd_create_dgram_qps(struct hrd_ctrl_blk* cb) {
  int i;
  assert(cb->dgram_qp != NULL && cb->dgram_send_cq != NULL &&
         cb->dgram_recv_cq != NULL && cb->pd != NULL && cb->ctx != NULL);
  assert(cb->num_dgram_qps >= 1 && cb->dev_port_id >= 1);

  for (i = 0; i < cb->num_dgram_qps; i++) {
    cb->dgram_send_cq[i] = ibv_create_cq(cb->ctx, HRD_Q_DEPTH, NULL, NULL, 0);
    assert(cb->dgram_send_cq[i] != NULL);

    cb->dgram_recv_cq[i] = ibv_create_cq(cb->ctx, HRD_Q_DEPTH, NULL, NULL, 0);
    assert(cb->dgram_recv_cq[i] != NULL);

    /* Initialize creation attributes */
    struct ibv_qp_init_attr create_attr;
    memset((void*)&create_attr, 0, sizeof(struct ibv_qp_init_attr));
    create_attr.send_cq = cb->dgram_send_cq[i];
    create_attr.recv_cq = cb->dgram_recv_cq[i];
    create_attr.qp_type = IBV_QPT_UD;

    create_attr.cap.max_send_wr = HRD_Q_DEPTH;
    create_attr.cap.max_recv_wr = HRD_Q_DEPTH;
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_recv_sge = 1;
    create_attr.cap.max_inline_data = HRD_MAX_INLINE;

    cb->dgram_qp[i] = ibv_create_qp(cb->pd, &create_attr);
    assert(cb->dgram_qp[i] != NULL);

    /* INIT state */
    struct ibv_qp_attr init_attr;
    memset((void*)&init_attr, 0, sizeof(struct ibv_qp_attr));
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.pkey_index = 0;
    init_attr.port_num = cb->dev_port_id;
    init_attr.qkey = HRD_DEFAULT_QKEY;

    if (ibv_modify_qp(
            cb->dgram_qp[i], &init_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
      fprintf(stderr, "Failed to modify dgram QP to INIT\n");
      return;
    }

    /* RTR state */
    struct ibv_qp_attr rtr_attr;
    memset((void*)&rtr_attr, 0, sizeof(struct ibv_qp_attr));
    rtr_attr.qp_state = IBV_QPS_RTR;

    if (ibv_modify_qp(cb->dgram_qp[i], &rtr_attr, IBV_QP_STATE)) {
      fprintf(stderr, "Failed to modify dgram QP to RTR\n");
      exit(-1);
    }

    /* Reuse rtr_attr for RTS */
    rtr_attr.qp_state = IBV_QPS_RTS;
    rtr_attr.sq_psn = HRD_DEFAULT_PSN;

    if (ibv_modify_qp(cb->dgram_qp[i], &rtr_attr,
                      IBV_QP_STATE | IBV_QP_SQ_PSN)) {
      fprintf(stderr, "Failed to modify dgram QP to RTS\n");
      exit(-1);
    }
  }
}

/* Create connected QPs and transition them to INIT */
void hrd_create_conn_qps(struct hrd_ctrl_blk* cb) {
  int i;
  assert(cb->conn_qp != NULL && cb->conn_cq != NULL && cb->pd != NULL &&
         cb->ctx != NULL);
  assert(cb->num_conn_qps >= 1 && cb->dev_port_id >= 1);

  for (i = 0; i < cb->num_conn_qps; i++) {
    cb->conn_cq[i] = ibv_create_cq(cb->ctx, HRD_Q_DEPTH, NULL, NULL, 0);
    assert(cb->conn_cq[i] != NULL);

#if HRD_CONNECT_IB_ATOMICS == 0
    struct ibv_qp_init_attr create_attr;
    memset(&create_attr, 0, sizeof(struct ibv_qp_init_attr));
    create_attr.send_cq = cb->conn_cq[i];
    create_attr.recv_cq = cb->conn_cq[i];
    create_attr.qp_type = cb->use_uc == 1 ? IBV_QPT_UC : IBV_QPT_RC;

    create_attr.cap.max_send_wr = HRD_Q_DEPTH;
    create_attr.cap.max_recv_wr = 1; /* We don't do RECVs on conn QPs */
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_recv_sge = 1;
    create_attr.cap.max_inline_data = HRD_MAX_INLINE;

    cb->conn_qp[i] = ibv_create_qp(cb->pd, &create_attr);
    assert(cb->conn_qp[i] != NULL);

    struct ibv_qp_attr init_attr;
    memset(&init_attr, 0, sizeof(struct ibv_qp_attr));
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.pkey_index = 0;
    init_attr.port_num = cb->dev_port_id;
    init_attr.qp_access_flags = cb->use_uc == 1 ? IBV_ACCESS_REMOTE_WRITE
                                                : IBV_ACCESS_REMOTE_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_ATOMIC;

    if (ibv_modify_qp(cb->conn_qp[i], &init_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS)) {
      fprintf(stderr, "Failed to modify conn QP to INIT\n");
      exit(-1);
    }
#endif

#if HRD_CONNECT_IB_ATOMICS == 1
    assert(cb->use_uc == 0); /* This is for atomics; no atomics on UC */
    struct ibv_exp_qp_init_attr create_attr;
    memset(&create_attr, 0, sizeof(struct ibv_exp_qp_init_attr));

    create_attr.pd = cb->pd;
    create_attr.send_cq = cb->conn_cq[i];
    create_attr.recv_cq = cb->conn_cq[i];
    create_attr.cap.max_send_wr = HRD_Q_DEPTH;
    create_attr.cap.max_recv_wr = 1; /* We don't do RECVs on conn QPs */
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_recv_sge = 1;
    create_attr.cap.max_inline_data = HRD_MAX_INLINE;
    create_attr.max_atomic_arg = 8;
    create_attr.exp_create_flags = IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY;
    create_attr.comp_mask = IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS |
                            IBV_EXP_QP_INIT_ATTR_PD |
                            IBV_EXP_QP_INIT_ATTR_ATOMICS_ARG;
    create_attr.qp_type = IBV_QPT_RC;

    cb->conn_qp[i] = ibv_exp_create_qp(cb->ctx, &create_attr);
    assert(cb->conn_qp[i] != NULL);

    struct ibv_exp_qp_attr init_attr;
    memset(&init_attr, 0, sizeof(struct ibv_exp_qp_attr));
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.pkey_index = 0;
    init_attr.port_num = cb->dev_port_id;
    init_attr.qp_access_flags = cb->use_uc == 1 ? IBV_ACCESS_REMOTE_WRITE
                                                : IBV_ACCESS_REMOTE_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_ATOMIC;

    if (ibv_exp_modify_qp(cb->conn_qp[i], &init_attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                              IBV_QP_ACCESS_FLAGS)) {
      fprintf(stderr, "Failed to modify conn QP to INIT\n");
      exit(-1);
    }
#endif
  }
}

/* Connects @cb's queue pair index @n to remote QP @remote_qp_attr */
void hrd_connect_qp(struct hrd_ctrl_blk* cb, int n,
                    struct hrd_qp_attr* remote_qp_attr) {
  assert(cb != NULL && remote_qp_attr != NULL);
  assert(n >= 0 && n < cb->num_conn_qps);
  assert(cb->conn_qp[n] != NULL);
  assert(cb->dev_port_id >= 1);

#if HRD_CONNECT_IB_ATOMICS == 0
  struct ibv_qp_attr conn_attr;
  memset(&conn_attr, 0, sizeof(struct ibv_qp_attr));
  conn_attr.qp_state = IBV_QPS_RTR;
  conn_attr.path_mtu = IBV_MTU_4096;
  conn_attr.dest_qp_num = remote_qp_attr->qpn;
  conn_attr.rq_psn = HRD_DEFAULT_PSN;

  conn_attr.ah_attr.is_global = 0;
  conn_attr.ah_attr.dlid = remote_qp_attr->lid;
  conn_attr.ah_attr.sl = 0;
  conn_attr.ah_attr.src_path_bits = 0;
  conn_attr.ah_attr.port_num = cb->dev_port_id; /* Local port! */

  int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                  IBV_QP_RQ_PSN;

  if (!cb->use_uc) {
    conn_attr.max_dest_rd_atomic = 16;
    conn_attr.min_rnr_timer = 12;
    rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  }

  if (ibv_modify_qp(cb->conn_qp[n], &conn_attr, rtr_flags)) {
    fprintf(stderr, "HRD: Failed to modify QP to RTR\n");
    assert(false);
  }

  memset(&conn_attr, 0, sizeof(conn_attr));
  conn_attr.qp_state = IBV_QPS_RTS;
  conn_attr.sq_psn = HRD_DEFAULT_PSN;

  int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

  if (!cb->use_uc) {
    conn_attr.timeout = 14;
    conn_attr.retry_cnt = 7;
    conn_attr.rnr_retry = 7;
    conn_attr.max_rd_atomic = 16;
    conn_attr.max_dest_rd_atomic = 16;
    rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                 IBV_QP_MAX_QP_RD_ATOMIC;
  }

  if (ibv_modify_qp(cb->conn_qp[n], &conn_attr, rts_flags)) {
    fprintf(stderr, "HRD: Failed to modify QP to RTS\n");
    assert(false);
  }
#endif

#if HRD_CONNECT_IB_ATOMICS == 1
  struct ibv_exp_qp_attr conn_attr;
  memset(&conn_attr, 0, sizeof(struct ibv_exp_qp_attr));
  conn_attr.qp_state = IBV_QPS_RTR;
  conn_attr.path_mtu = IBV_MTU_4096;
  conn_attr.dest_qp_num = remote_qp_attr->qpn;
  conn_attr.rq_psn = HRD_DEFAULT_PSN;

  conn_attr.ah_attr.is_global = 0;
  conn_attr.ah_attr.dlid = remote_qp_attr->lid;
  conn_attr.ah_attr.sl = 0;
  conn_attr.ah_attr.src_path_bits = 0;
  conn_attr.ah_attr.port_num = cb->dev_port_id; /* Local port! */

  int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                  IBV_QP_RQ_PSN;

  if (!cb->use_uc) {
    conn_attr.max_dest_rd_atomic = 16;
    conn_attr.min_rnr_timer = 12;
    rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  }

  if (ibv_exp_modify_qp(cb->conn_qp[n], &conn_attr, rtr_flags)) {
    fprintf(stderr, "HRD: Failed to modify QP to RTR\n");
    assert(false);
  }

  memset(&conn_attr, 0, sizeof(conn_attr));
  conn_attr.qp_state = IBV_QPS_RTS;
  conn_attr.sq_psn = HRD_DEFAULT_PSN;

  int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

  if (!cb->use_uc) {
    conn_attr.timeout = 14;
    conn_attr.retry_cnt = 7;
    conn_attr.rnr_retry = 7;
    conn_attr.max_rd_atomic = 16;
    conn_attr.max_dest_rd_atomic = 16;
    rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                 IBV_QP_MAX_QP_RD_ATOMIC;
  }

  if (ibv_exp_modify_qp(cb->conn_qp[n], &conn_attr, rts_flags)) {
    fprintf(stderr, "HRD: Failed to modify QP to RTS\n");
    assert(false);
  }
#endif

  return;
}

void hrd_publish_conn_qp(struct hrd_ctrl_blk* cb, int n, const char* qp_name) {
  assert(cb != NULL);
  assert(n >= 0 && n < cb->num_conn_qps);

  assert(qp_name != NULL && strlen(qp_name) < HRD_QP_NAME_SIZE - 1);
  assert(strstr(qp_name, HRD_RESERVED_NAME_PREFIX) == NULL);

  int len = strlen(qp_name);
  int i;
  for (i = 0; i < len; i++) {
    if (qp_name[i] == ' ') {
      fprintf(stderr, "HRD: Space not allowed in QP name\n");
      exit(-1);
    }
  }

  struct hrd_qp_attr qp_attr;
  memcpy(qp_attr.name, qp_name, len);
  qp_attr.name[len] = 0; /* Add the null terminator */
  qp_attr.buf_addr = (uint64_t)(uintptr_t)cb->conn_buf;
  qp_attr.buf_size = cb->conn_buf_size;
  qp_attr.rkey = cb->conn_buf_mr->rkey;
  qp_attr.lid = hrd_get_local_lid(cb->conn_qp[n]->context, cb->dev_port_id);
  qp_attr.qpn = cb->conn_qp[n]->qp_num;

  hrd_publish(qp_attr.name, &qp_attr, sizeof(struct hrd_qp_attr));
}

void hrd_publish_dgram_qp(struct hrd_ctrl_blk* cb, int n, const char* qp_name) {
  assert(cb != NULL);
  assert(n >= 0 && n < cb->num_dgram_qps);

  assert(qp_name != NULL && strlen(qp_name) < HRD_QP_NAME_SIZE - 1);
  assert(strstr(qp_name, HRD_RESERVED_NAME_PREFIX) == NULL);

  int len = strlen(qp_name);
  int i;
  for (i = 0; i < len; i++) {
    if (qp_name[i] == ' ') {
      fprintf(stderr, "HRD: Space not allowed in QP name\n");
      exit(-1);
    }
  }

  struct hrd_qp_attr qp_attr;
  memcpy(qp_attr.name, qp_name, len);
  qp_attr.name[len] = 0; /* Add the null terminator */
  qp_attr.lid = hrd_get_local_lid(cb->dgram_qp[n]->context, cb->dev_port_id);
  qp_attr.qpn = cb->dgram_qp[n]->qp_num;

  hrd_publish(qp_attr.name, &qp_attr, sizeof(struct hrd_qp_attr));
}

struct hrd_qp_attr* hrd_get_published_qp(const char* qp_name) {
  struct hrd_qp_attr* ret;
  assert(qp_name != NULL && strlen(qp_name) < HRD_QP_NAME_SIZE - 1);
  assert(strstr(qp_name, HRD_RESERVED_NAME_PREFIX) == NULL);

  int len = strlen(qp_name);
  int i;
  for (i = 0; i < len; i++) {
    if (qp_name[i] == ' ') {
      fprintf(stderr, "HRD: Space not allowed in QP name\n");
      exit(-1);
    }
  }

  int ret_len = hrd_get_published(qp_name, (void**)&ret);

  /*
   * The registry lookup returns only if we get a unique QP for @qp_name, or
   * if the memcached lookup succeeds but we don't have an entry for @qp_name.
   */
  assert(ret_len == sizeof(struct hrd_qp_attr) || ret_len == -1);

  return ret;
}
