#include <signal.h>
#include "main.h"

__thread FILE* out_fp = NULL; /* File to record throughput */
__thread struct hrd_ctrl_blk* cb;
__thread int machine_id;
__thread int wrkr_gid;
__thread int wrkr_lid;

/* Is the remote QP for a local QP on the same physical machine? */
inline int is_remote_qp_on_same_physical_mc(int qp_i) {
  return (qp_i % NUM_MACHINES == machine_id);
}

/* Get the name for the QP index qp_i created by this thread */
void get_qp_name_local(char* namebuf, int qp_i) {
  assert(namebuf != NULL);
  assert(qp_i >= 0 && qp_i < NUM_QPS_PER_THREAD);

  int for_phys_mc = qp_i % NUM_MACHINES;
  int for_vm = qp_i / NUM_MACHINES;

  sprintf(namebuf, "on_phys_mc_%d-at_thr_%d-for_phys_mc_%d-for_vm_%d",
          machine_id, wrkr_lid, for_phys_mc, for_vm);
}

/*
 * Get the name of the remote QP that QP index qp_i created by this thread
 * should connect to.
 */
void get_qp_name_remote(char* namebuf, int qp_i) {
  assert(namebuf != NULL);
  assert(qp_i >= 0 && qp_i < NUM_QPS_PER_THREAD);

  int for_phys_mc = qp_i % NUM_MACHINES;
  int for_vm = qp_i / NUM_MACHINES;

  sprintf(namebuf, "on_phys_mc_%d-at_thr_%d-for_phys_mc_%d-for_vm_%d",
          for_phys_mc, wrkr_lid, machine_id, for_vm);
}

/* Record machine throughput */
void record_sweep_params() {
  fprintf(out_fp, "Machine %d: sweep parameters: ", machine_id);
  fprintf(out_fp, "SIZE %d, ", SIZE);
  fprintf(out_fp, "WINDOW_SIZE %d, ", WINDOW_SIZE);
  fprintf(out_fp, "UNSIG_BATCH %d, ", UNSIG_BATCH);
  fprintf(out_fp, "ALLSIG %d, ", ALLSIG);
  fprintf(out_fp, "NUM_WORKERS %d, ", NUM_WORKERS);
  if (ACTIVE_MODE == MODE_INBOUND) {
    fprintf(out_fp, "ACTIVE_MODE MODE_INBOUND\n");
  } else if (ACTIVE_MODE == MODE_OUTBOUND) {
    fprintf(out_fp, "ACTIVE_MODE MODE_OUTBOUND\n");
  } else if (ACTIVE_MODE == MODE_SWARM) {
    fprintf(out_fp, "ACTIVE_MODE MODE_SWARM\n");
  }
  fflush(out_fp);
}

/* Record machine throughput */
void record_machine_tput(double total_tput) {
  assert(out_fp != NULL);
  char timebuf[50];
  hrd_get_formatted_time(timebuf);

  fprintf(out_fp, "Machine %d: tput = %.2f reqs/s, time %s\n", machine_id,
          total_tput, timebuf);
  fflush(out_fp);
}

/*
 * In inbound mode, the machine #0 is passive. In outbound mode, all remote
 * machines are passive.
 */
void sleep_if_inbound_outbound() {
  if (ACTIVE_MODE == MODE_INBOUND && machine_id == 0) {
    sleep(100000000);
  }

  if (ACTIVE_MODE == MODE_OUTBOUND && machine_id != 0) {
    sleep(100000000);
  }
}

/* Choose a QP to send an RDMA on */
static inline int choose_qp(uint64_t* seed) {
  int qp_i;

#if ACTIVE_MODE == MODE_INBOUND
  /* Choose a virtual machine on the 1st machine */
  assert(false); /* XXX: Figure this out */
#elif ACTIVE_MODE == MODE_OUTBOUND
  /* Choose a virtual machine on a server other than the 1st machine */
  assert(false); /* Figure this out */
#elif ACTIVE_MODE == MODE_SWARM
  qp_i = hrd_fastrand(seed) % NUM_QPS_PER_THREAD;
  while (is_remote_qp_on_same_physical_mc(qp_i)) {
    qp_i = hrd_fastrand(seed) % NUM_QPS_PER_THREAD;
  }
#endif

  return qp_i;
}

void kill_handler(int sig) {
  printf("Destroying control block for worker %d\n", wrkr_gid);
  hrd_ctrl_blk_destroy(cb);
}

void* run_worker(void* arg) {
  int i, ret;
  signal(SIGINT, kill_handler);
  signal(SIGKILL, kill_handler);
  signal(SIGTERM, kill_handler);

  struct thread_params params = *(struct thread_params*)arg;

  /* Record some globals */
  wrkr_gid = params.wrkr_gid; /* Global ID of this thread */
  wrkr_lid = params.wrkr_lid; /* Local ID of this thread */
  assert(wrkr_lid == wrkr_gid % NUM_THREADS);
  machine_id = wrkr_gid / NUM_THREADS;
  assert(machine_id == params.machine_id);

  int base_port_index = params.base_port_index;

  int num_ports = params.num_ports;
  assert(num_ports <= MAX_PORTS); /* Avoid dynamic alloc */

  int first_in_machine = (wrkr_gid % NUM_THREADS == 0);

  printf("Worker %d: use_uc = %d\n", wrkr_gid, params.use_uc);

  int size = params.size;
  assert(size <= BUF_SIZE);

  /* We do not memcpy if DO_MEMCPY_ON_READ_COMPLETION == 0 */
  uint8_t* memcpy_buf __attribute__((unused)) = malloc(size);
  assert(memcpy_buf != NULL);

  int vport_index = wrkr_lid % num_ports;
  int ib_port_index = base_port_index + vport_index;

  /* Create the output file for this machine */
  if (first_in_machine == 1) {
    char filename[100];
    sprintf(filename, "tput-out/machine-%d", machine_id);
    out_fp = fopen(filename, "w");
    assert(out_fp != NULL);
    record_sweep_params(machine_id);
  }

  /* Create the control block */
  int wrkr_shm_key = WORKER_BASE_SHM_KEY + (wrkr_gid % NUM_THREADS);
  cb = hrd_ctrl_blk_init(wrkr_gid,         /* local_hid */
                         ib_port_index, 0, /* port_index, numa_node_id */
                         NUM_QPS_PER_THREAD, params.use_uc, /* conn qps, uc */
                         NULL, BUF_SIZE,
                         wrkr_shm_key, /* prealloc buf, size, key */
                         0, 0, -1);    /* #dgram qps, buf size, shm key */

  if (params.do_read == 1) {
    /* Set to 0 so that we can detect READ completion by polling. */
    memset((void*)cb->conn_buf, 0, BUF_SIZE);
  } else {
    /* Set to 5 so that WRITEs show up as a weird value */
    memset((void*)cb->conn_buf, 5, BUF_SIZE);
  }

  /* Publish worker QPs */
  for (i = 0; i < NUM_QPS_PER_THREAD; i++) {
    char local_qp_name[HRD_QP_NAME_SIZE];
    get_qp_name_local(local_qp_name, i);

    hrd_publish_conn_qp(cb, i, local_qp_name);
  }
  printf("main: Worker %d published local QPs\n", wrkr_gid);

  /* Find QPs to connect to */
  struct hrd_qp_attr* remote_qp_arr[NUM_QPS_PER_THREAD] = {NULL};
  for (i = 0; i < NUM_QPS_PER_THREAD; i++) {
    /* Do not connect if remote QP is on this machine */
    if (is_remote_qp_on_same_physical_mc(i)) {
      continue;
    }

    char remote_qp_name[HRD_QP_NAME_SIZE];
    get_qp_name_remote(remote_qp_name, i);

    printf("main: Worker %d looking for %s.\n", wrkr_gid, remote_qp_name);
    while (remote_qp_arr[i] == NULL) {
      remote_qp_arr[i] = hrd_get_published_qp(remote_qp_name);
      if (remote_qp_arr[i] == NULL) {
        usleep(20000);
      }
    }

    printf("main: Worker %d found %s! Connecting..\n", wrkr_gid,
           remote_qp_name);
    hrd_connect_qp(cb, i, remote_qp_arr[i]);

    char local_qp_name[HRD_QP_NAME_SIZE];
    get_qp_name_local(local_qp_name, i);
    hrd_publish_ready(local_qp_name);
  }

  for (i = 0; i < NUM_QPS_PER_THREAD; i++) {
    /* Do not connect if remote QP is on this machine */
    if (is_remote_qp_on_same_physical_mc(i)) {
      continue;
    }

    char remote_qp_name[HRD_QP_NAME_SIZE];
    get_qp_name_remote(remote_qp_name, i);

    printf("main: Worker %d waiting for %s to get ready\n", wrkr_gid,
           remote_qp_name);
    hrd_wait_till_ready(remote_qp_name);
  }

  printf("main: Worker %d ready\n", wrkr_gid);

  /* This needs to be done after connecting with remote QPs */
  sleep_if_inbound_outbound(machine_id);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc;
  long long rolling_iter = 0;                /* For performance measurement */
  long long nb_tx[NUM_QPS_PER_THREAD] = {0}; /* Per-QP tracking for signaling */
  long long nb_tx_tot = 0; /* For windowing (for READs only) */
  int window_i = 0;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  int opcode = params.do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  int qpn = 0; /* Queue pair number to read or write from */
  int rec_qpn_arr[WINDOW_SIZE] = {0}; /* Record which QP we used */

  /* Move fastrand for this worker */
  uint64_t seed __attribute__((unused)) = 0xdeadbeef;
  for (i = 0; i < wrkr_gid * 10000000; i++) {
    hrd_fastrand(&seed);
  }

  while (1) {
    if (rolling_iter >= M_2) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      double tput = rolling_iter / seconds;

      printf(
          "main: Worker %d: %.2f Mops. Total active QPs = %d. "
          "Outstanding ops per thread (for READs) = %d. "
          "Channel offset #0: %lld, memcpy_buf #0: %u\n",
          wrkr_gid, tput, NUM_THREADS * NUM_QPS_PER_THREAD, WINDOW_SIZE,
          params.cnoff_arr[0].offset, memcpy_buf[0]);

      /* Per-machine throughput computation */
      params.tput_arr[wrkr_gid % NUM_THREADS] = tput;
      if (first_in_machine == 1) {
        double machine_tput = 0;
        for (i = 0; i < NUM_THREADS; i++) {
          machine_tput += params.tput_arr[i];
        }
        record_machine_tput(machine_tput);
        hrd_red_printf("main: Total tput %.2f Mops\n", machine_tput);
      }
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /*
     * For READs and ALLSIG WRITEs, we can restrict outstanding ops per
     * thread to WINDOW_SIZE.
     */
    if (nb_tx_tot >= WINDOW_SIZE) {
      if (ALLSIG == 0 && opcode == IBV_WR_RDMA_READ) {
        while (cb->conn_buf[window_i * size] == 0) {
          /* Wait for a window slow to open up */
        }

        /* Zero-out the slot for this round */
        cb->conn_buf[window_i * size] = 0;

#if DO_MEMCPY_ON_READ_COMPLETION
        memcpy(memcpy_buf, (void*)&cb->conn_buf[window_i * size], size);
#endif
      } else if (ALLSIG == 1) {
        int qpn_to_poll = rec_qpn_arr[window_i];
        int ret = hrd_poll_cq_ret(cb->conn_cq[qpn_to_poll], 1, &wc);
        // printf("Polled window index = %d\n", window_i);
        if (ret != 0) {
          printf("Worker %d destroying cb and exiting\n", wrkr_gid);
          hrd_ctrl_blk_destroy(cb);
          exit(-1);
        }
      }
    }

    /* Choose the next machine to send RDMA to and record it */
    qpn = choose_qp(&seed);
    rec_qpn_arr[window_i] = qpn;

    wr.opcode = opcode;
    wr.num_sge = 1;
    wr.next = NULL;
    wr.sg_list = &sgl;

#if ALLSIG == 0
    wr.send_flags = (nb_tx[qpn] & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
    if ((nb_tx[qpn] & UNSIG_BATCH_) == UNSIG_BATCH_) {
      int ret = hrd_poll_cq_ret(cb->conn_cq[qpn], 1, &wc);
      if (ret != 0) {
        printf("Worker %d destroying cb and exiting\n", wrkr_gid);
        hrd_ctrl_blk_destroy(cb);
        exit(-1);
      }
    }
#else
    wr.send_flags = IBV_SEND_SIGNALED;
#endif
    nb_tx[qpn]++;

    wr.send_flags |= (params.do_read == 0) ? IBV_SEND_INLINE : 0;

    /*
     * Aligning local/remote offset to 64-byte boundary REDUCES performance
     * significantly (similar to atomics).
     */
    int _offset = (hrd_fastrand(&seed) & BUF_SIZE_);
    while (_offset >= BUF_SIZE - size) {
      _offset = (hrd_fastrand(&seed) & BUF_SIZE_);
    }

#if ALLSIG == 0
    /* Use a predictable address to make polling easy */
    sgl.addr = (uint64_t)(uintptr_t)&cb->conn_buf[window_i * size];
#else
    /* We'll use CQE to detect comp; using random address improves perf */
    sgl.addr = (uint64_t)(uintptr_t)&cb->conn_buf[_offset];
#endif

    sgl.length = size;
    sgl.lkey = cb->conn_buf_mr->lkey;

    wr.wr.rdma.remote_addr = remote_qp_arr[qpn]->buf_addr + _offset;
    wr.wr.rdma.rkey = remote_qp_arr[qpn]->rkey;

// printf("Worker %d: Sending request %lld to over QP %d.\n",
//	wrkr_gid, nb_tx_tot, qpn);

#if ENABLE_SHARED_CHANNELS == 1
    int rand_channel = hrd_fastrand(&seed) & NUM_CHANNELS_;
    __sync_fetch_and_add(&params.cnoff_arr[rand_channel].offset, 1);
#endif

    ret = ibv_post_send(cb->conn_qp[qpn], &wr, &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    rolling_iter++;
    nb_tx_tot++;
    HRD_MOD_ADD(window_i, WINDOW_SIZE);
  }

  return NULL;
}
