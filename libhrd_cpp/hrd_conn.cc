#include "hrd.h"

/*
 * If @prealloc_conn_buf != nullptr, @conn_buf_size is the size of the
 * preallocated buffer. If @prealloc_conn_buf == nullptr, @conn_buf_size is the
 * size of the new buffer to create.
 */
struct hrd_ctrl_blk_t* hrd_ctrl_blk_init(size_t local_hid, size_t port_index,
                                         size_t numa_node_id,
                                         hrd_conn_config_t* conn_config,
                                         hrd_dgram_config_t* dgram_config) {
#if HRD_CONNECT_IB_ATOMICS == 1
  hrd_red_printf(
      "HRD: Connect-IB atomics enabled. This QP setup has not "
      "been tested for non-atomics performance.\n");
  sleep(1);
#endif

  hrd_red_printf("HRD: creating control block %zu: port %zu, socket %zu.\n",
                 local_hid, port_index, numa_node_id);

  if (conn_config != nullptr) {
    hrd_red_printf(
        "HRD: control block %zu: "
        "conn qps %zu UC %d, conn buf %zu bytes (key %d).\n",
        local_hid, conn_config->num_qps, conn_config->use_uc,
        conn_config->buf_size, conn_config->buf_shm_key);
  }

  if (dgram_config != nullptr) {
    hrd_red_printf(
        "HRD: control block %zu: "
        "dgram qps %zu, dgram buf %.3f MB (key %d)\n",
        local_hid, dgram_config->num_qps, dgram_config->buf_size / M_1 * 1.0,
        dgram_config->buf_shm_key);
  }

  /*
   * Check arguments for sanity.
   * @local_hid can be anything: it's just control block identifier that is
   * useful in printing debug info.
   */
  assert(port_index <= 16);
  assert(numa_node_id <= 8);
  assert(num_conn_qps >= 0);
  assert(conn_buf_size <= M_1024);

  /* If there is no preallocated buffer, shm key can be =/>/< 0 */
  if (prealloc_conn_buf != nullptr) {
    assert(conn_buf_shm_key == -1);
  }
  if (prealloc_dgram_buf != nullptr) {
    assert(dgram_buf_shm_key == -1);
  }
  assert(num_dgram_qps <= M_2);
  assert(dgram_buf_size <= M_1024);
  assert(num_conn_qps > 0 || num_dgram_qps > 0);

  auto* cb = new hrd_ctrl_blk_t();
  memset(cb, 0, sizeof(hrd_ctrl_blk_t));

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
    cb->dgram_qp = new ibv_qp*[num_dgram_qps];
    cb->dgram_send_cq = new ibv_cq*[num_dgram_qps];
    cb->dgram_recv_cq = new ibv_cq*[num_dgram_qps];

    hrd_create_dgram_qps(cb);

    if (prealloc_dgram_buf == nullptr) {
      /* Create and register dgram_buf - always make it multiple of 2 MB */
      size_t reg_size = 0;

      if (numa_node_id <= 8) {
        /* Hugepages */
        while (reg_size < dgram_buf_size) {
          reg_size += M_2;
        }

        /* SHM key 0 is hard to free later */
        assert(dgram_buf_shm_key >= 1 && dgram_buf_shm_key <= 128);
        cb->dgram_buf = reinterpret_cast<volatile uint8_t*>(
            hrd_malloc_socket(dgram_buf_shm_key, reg_size, numa_node_id));
      } else {
        reg_size = dgram_buf_size;
        cb->dgram_buf =
            reinterpret_cast<volatile uint8_t*>(memalign(4096, reg_size));
      }

      assert(cb->dgram_buf != nullptr);
      memset(const_cast<uint8_t*>(cb->dgram_buf), 0, reg_size);

      cb->dgram_buf_mr = ibv_reg_mr(cb->pd, const_cast<uint8_t*>(cb->dgram_buf),
                                    reg_size, ib_flags);
      assert(cb->dgram_buf_mr != nullptr);
    } else {
      cb->dgram_buf = const_cast<volatile uint8_t*>(prealloc_dgram_buf);
      cb->dgram_buf_mr = ibv_reg_mr(cb->pd, const_cast<uint8_t*>(cb->dgram_buf),
                                    cb->dgram_buf_size, ib_flags);
      assert(cb->dgram_buf_mr != nullptr);
    }
  }

  /*
   * Create connected QPs and transition them to RTS.
   * Create and register connected QP RDMA buffer.
   */
  if (num_conn_qps >= 1) {
    cb->conn_qp = new ibv_qp*[num_conn_qps];
    cb->conn_cq = new ibv_cq*[num_conn_qps];

    hrd_create_conn_qps(cb);

    if (prealloc_conn_buf == nullptr) {
      /* Create and register conn_buf - always make it multiple of 2 MB*/
      size_t reg_size = 0;

      /* If numa_node_id < 0, use standard heap memory */
      if (numa_node_id <= 8) {
        /* Hugepages */
        while (reg_size < conn_buf_size) {
          reg_size += M_2;
        }

        /* SHM key 0 is hard to free later */
        assert(conn_buf_shm_key >= 1 && conn_buf_shm_key <= 128);
        cb->conn_buf = reinterpret_cast<volatile uint8_t*>(
            hrd_malloc_socket(conn_buf_shm_key, reg_size, numa_node_id));
      } else {
        reg_size = conn_buf_size;
        cb->conn_buf =
            reinterpret_cast<volatile uint8_t*>(memalign(4096, reg_size));
        assert(cb->conn_buf != nullptr);
      }
      memset(const_cast<uint8_t*>(cb->conn_buf), 0, reg_size);
      cb->conn_buf_mr = ibv_reg_mr(cb->pd, const_cast<uint8_t*>(cb->conn_buf),
                                   reg_size, ib_flags);
      assert(cb->conn_buf_mr != nullptr);
    } else {
      cb->conn_buf = const_cast<volatile uint8_t*>(prealloc_conn_buf);
      cb->conn_buf_mr = ibv_reg_mr(cb->pd, const_cast<uint8_t*>(cb->conn_buf),
                                   cb->conn_buf_size, ib_flags);
      assert(cb->conn_buf_mr != nullptr);
    }
  }

  return cb;
}

/* Free up the resources taken by @cb. Return -1 if something fails, else 0. */
int hrd_ctrl_blk_destroy(hrd_ctrl_blk_t* cb) {
  hrd_red_printf("HRD: Destroying control block %d\n", cb->local_hid);

  /* Destroy QPs and CQs. QPs must be destroyed before CQs. */
  for (size_t i = 0; i < cb->num_dgram_qps; i++) {
    assert(cb->dgram_send_cq[i] != nullptr && cb->dgram_recv_cq[i] != nullptr);
    assert(cb->dgram_qp[i] != nullptr);

    if (ibv_destroy_qp(cb->dgram_qp[i])) {
      fprintf(stderr, "HRD: Couldn't destroy dgram QP %zu\n", i);
      return -1;
    }

    if (ibv_destroy_cq(cb->dgram_send_cq[i])) {
      fprintf(stderr, "HRD: Couldn't destroy dgram SEND CQ %zu\n", i);
      return -1;
    }

    if (ibv_destroy_cq(cb->dgram_recv_cq[i])) {
      fprintf(stderr, "HRD: Couldn't destroy dgram RECV CQ %zu\n", i);
      return -1;
    }
  }

  for (size_t i = 0; i < cb->num_conn_qps; i++) {
    assert(cb->conn_cq[i] != nullptr && cb->conn_qp[i] != nullptr);

    if (ibv_destroy_qp(cb->conn_qp[i])) {
      fprintf(stderr, "HRD: Couldn't destroy conn QP %zud\n", i);
      return -1;
    }

    if (ibv_destroy_cq(cb->conn_cq[i])) {
      fprintf(stderr, "HRD: Couldn't destroy conn CQ %zud\n", i);
      return -1;
    }
  }

  /* Destroy memory regions */
  if (cb->num_dgram_qps > 0) {
    assert(cb->dgram_buf_mr != nullptr && cb->dgram_buf != nullptr);
    if (ibv_dereg_mr(cb->dgram_buf_mr)) {
      fprintf(stderr, "HRD: Couldn't deregister dgram MR for cb %zu\n",
              cb->local_hid);
      return -1;
    }

    if (cb->numa_node_id <= 8) {
      if (hrd_free(cb->dgram_buf_shm_key,
                   const_cast<uint8_t*>(cb->dgram_buf))) {
        fprintf(stderr, "HRD: Error freeing dgram hugepages for cb %zu.\n",
                cb->local_hid);
      }
    } else {
      free(const_cast<uint8_t*>(cb->dgram_buf));
    }
  }

  if (cb->num_conn_qps > 0) {
    assert(cb->conn_buf_mr != nullptr);
    if (ibv_dereg_mr(cb->conn_buf_mr)) {
      fprintf(stderr, "HRD: Couldn't deregister conn MR for cb %zu\n",
              cb->local_hid);
      return -1;
    }

    if (cb->numa_node_id <= 8) {
      if (hrd_free(cb->conn_buf_shm_key, const_cast<uint8_t*>(cb->conn_buf))) {
        fprintf(stderr, "HRD: Error freeing conn hugepages for cb %zu\n",
                cb->local_hid);
      }
    } else {
      free(const_cast<uint8_t*>(cb->conn_buf));
    }
  }

  /* Destroy protection domain */
  if (ibv_dealloc_pd(cb->pd)) {
    fprintf(stderr, "HRD: Couldn't dealloc PD for cb %zu\n", cb->local_hid);
    return -1;
  }

  /* Destroy device context */
  if (ibv_close_device(cb->ctx)) {
    fprintf(stderr, "Couldn't release context for cb %zu\n", cb->local_hid);
    return -1;
  }

  hrd_red_printf("HRD: Control block %d destroyed.\n", cb->local_hid);
  return 0;
}

/* Create datagram QPs and transition them to RTS */
void hrd_create_dgram_qps(hrd_ctrl_blk_t* cb) {
  assert(cb->dgram_qp != nullptr && cb->dgram_send_cq != nullptr &&
         cb->dgram_recv_cq != nullptr && cb->pd != nullptr &&
         cb->ctx != nullptr);
  assert(cb->num_dgram_qps >= 1 && cb->dev_port_id >= 1);

  for (size_t i = 0; i < cb->num_dgram_qps; i++) {
    size_t recv_queue_depth = (i == 0) ? HRD_RQ_DEPTH : 1;

    cb->dgram_send_cq[i] =
        ibv_create_cq(cb->ctx, HRD_SQ_DEPTH, nullptr, nullptr, 0);
    assert(cb->dgram_send_cq[i] != nullptr);

    cb->dgram_recv_cq[i] =
        ibv_create_cq(cb->ctx, recv_queue_depth, nullptr, nullptr, 0);
    assert(cb->dgram_recv_cq[i] != nullptr);

    /* Initialize creation attributes */
    struct ibv_qp_init_attr create_attr;
    memset(&create_attr, 0, sizeof(struct ibv_qp_init_attr));
    create_attr.send_cq = cb->dgram_send_cq[i];
    create_attr.recv_cq = cb->dgram_recv_cq[i];
    create_attr.qp_type = IBV_QPT_UD;

    create_attr.cap.max_send_wr = HRD_SQ_DEPTH;
    create_attr.cap.max_recv_wr = recv_queue_depth;
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_recv_sge = 1;
    create_attr.cap.max_inline_data = HRD_MAX_INLINE;

    cb->dgram_qp[i] = ibv_create_qp(cb->pd, &create_attr);
    assert(cb->dgram_qp[i] != nullptr);

    /* INIT state */
    struct ibv_qp_attr init_attr;
    memset(&init_attr, 0, sizeof(struct ibv_qp_attr));
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
    memset(&rtr_attr, 0, sizeof(struct ibv_qp_attr));
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
void hrd_create_conn_qps(hrd_ctrl_blk_t* cb) {
  assert(cb->conn_qp != nullptr && cb->conn_cq != nullptr &&
         cb->pd != nullptr && cb->ctx != nullptr);
  assert(cb->num_conn_qps >= 1 && cb->dev_port_id >= 1);

  for (size_t i = 0; i < cb->num_conn_qps; i++) {
    cb->conn_cq[i] = ibv_create_cq(cb->ctx, HRD_SQ_DEPTH, nullptr, nullptr, 0);
    assert(cb->conn_cq[i] != nullptr);

#if HRD_CONNECT_IB_ATOMICS == 0
    struct ibv_qp_init_attr create_attr;
    memset(&create_attr, 0, sizeof(struct ibv_qp_init_attr));
    create_attr.send_cq = cb->conn_cq[i];
    create_attr.recv_cq = cb->conn_cq[i];
    create_attr.qp_type = cb->use_uc == 1 ? IBV_QPT_UC : IBV_QPT_RC;

    create_attr.cap.max_send_wr = HRD_SQ_DEPTH;
    create_attr.cap.max_recv_wr = 1; /* We don't do RECVs on conn QPs */
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_recv_sge = 1;
    create_attr.cap.max_inline_data = HRD_MAX_INLINE;

    cb->conn_qp[i] = ibv_create_qp(cb->pd, &create_attr);
    assert(cb->conn_qp[i] != nullptr);

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
    create_attr.cap.max_send_wr = HRD_SQ_DEPTH;
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
    assert(cb->conn_qp[i] != nullptr);

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
void hrd_connect_qp(hrd_ctrl_blk_t* cb, size_t n,
                    hrd_qp_attr_t* remote_qp_attr) {
  assert(cb != nullptr && remote_qp_attr != nullptr);
  assert(n < cb->num_conn_qps);
  assert(cb->conn_qp[n] != nullptr);
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
  conn_attr.ah_attr..dlid = remote_qp_attr->lid;
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

void hrd_publish_conn_qp(hrd_ctrl_blk_t* cb, size_t n, const char* qp_name) {
  assert(n < cb->num_conn_qps);
  assert(strlen(qp_name) < HRD_QP_NAME_SIZE - 1);
  assert(strstr(qp_name, HRD_RESERVED_NAME_PREFIX) == nullptr);

  size_t len = strlen(qp_name);
  for (size_t i = 0; i < len; i++) assert(qp_name[i] != ' ');

  hrd_qp_attr_t qp_attr;
  memcpy(qp_attr.name, qp_name, len);
  qp_attr.name[len] = 0; /* Add the null terminator */
  qp_attr.buf_addr = reinterpret_cast<uint64_t>(cb->conn_buf);
  qp_attr.buf_size = cb->conn_buf_size;
  qp_attr.rkey = cb->conn_buf_mr->rkey;
  qp_attr.lid = hrd_get_local_lid(cb->conn_qp[n]->context, cb->dev_port_id);
  qp_attr.qpn = cb->conn_qp[n]->qp_num;

  hrd_publish(qp_attr.name, &qp_attr, sizeof(hrd_qp_attr_t));
}

void hrd_publish_dgram_qp(hrd_ctrl_blk_t* cb, size_t n, const char* qp_name) {
  assert(n < cb->num_dgram_qps);
  assert(strlen(qp_name) < HRD_QP_NAME_SIZE - 1);
  assert(strstr(qp_name, HRD_RESERVED_NAME_PREFIX) == nullptr);

  size_t len = strlen(qp_name);
  for (size_t i = 0; i < len; i++) assert(qp_name[i] != ' ');

  hrd_qp_attr_t qp_attr;
  memcpy(qp_attr.name, qp_name, len);
  qp_attr.name[len] = 0; /* Add the null terminator */
  qp_attr.lid = hrd_get_local_lid(cb->dgram_qp[n]->context, cb->dev_port_id);
  qp_attr.qpn = cb->dgram_qp[n]->qp_num;

  hrd_publish(qp_attr.name, &qp_attr, sizeof(hrd_qp_attr_t));
}

hrd_qp_attr_t* hrd_get_published_qp(const char* qp_name) {
  assert(strlen(qp_name) < HRD_QP_NAME_SIZE - 1);
  assert(strstr(qp_name, HRD_RESERVED_NAME_PREFIX) == nullptr);

  hrd_qp_attr_t* ret;
  for (size_t i = 0; i < strlen(qp_name); i++) assert(qp_name[i] != ' ');

  int ret_len = hrd_get_published(qp_name, reinterpret_cast<void**>(&ret));

  /*
   * The registry lookup returns only if we get a unique QP for @qp_name, or
   * if the memcached lookup succeeds but we don't have an entry for @qp_name.
   */
  assert(ret_len == sizeof(hrd_qp_attr_t) || ret_len == -1);
  _unused(ret_len);

  return ret;
}
