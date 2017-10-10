#include "hrd.h"
#include "main.h"
#include "mica.h"

/* Random local_hids for master */
#define MASTER_P0_ID -22

void* run_master(void* arg) {
  int i, j;
  struct thread_params master_params = *(struct thread_params*)arg;
  int num_server_ports = master_params.num_server_ports;
  int base_port_index = master_params.base_port_index;

  hrd_red_printf(
      "Running HERD master with num_server_ports = %d "
      "base_port_index = %d, RR_SIZE = %d MB, RR use fraction = %.2f\n",
      num_server_ports, base_port_index, RR_SIZE / M_1,
      (double)sizeof(struct mica_op) * NUM_CLIENTS * NUM_WORKERS * WINDOW_SIZE /
          RR_SIZE);

  /*
   * Create one control block per port. Each control block has enough QPs
   * for all client QPs. The ith client QP must connect to the ith QP in a
   * a control block, but clients can choose the control block.
   */
  struct hrd_ctrl_blk** cb = malloc(num_server_ports * sizeof(void*));
  assert(cb != NULL);

  /* Allocate a registered buffer for port #0 only */
  for (i = 0; i < num_server_ports; i++) {
    volatile void* prealloc_conn_buf = (i == 0 ? NULL : cb[0]->conn_buf);
    int ib_port_index = base_port_index + i;
    int shm_key = (i == 0 ? MASTER_SHM_KEY : -1);

    cb[i] = hrd_ctrl_blk_init(MASTER_P0_ID + i, /* local hid */
                              ib_port_index, 0, /* port index, numa node id */
                              NUM_CLIENTS, 1,   /* #conn_qps, use_uc */
                              prealloc_conn_buf, RR_SIZE, shm_key, 0, 0,
                              -1); /* #dgram qps, buf size, shm key */

    /* Zero out the request buffer */
    memset((void*)cb[i]->conn_buf, 0, RR_SIZE);

    /* Register all created QPs - only some will get used! */
    for (j = 0; j < NUM_CLIENTS; j++) {
      char srv_name[HRD_QP_NAME_SIZE];
      sprintf(srv_name, "master-%d-%d", i, j);
      hrd_publish_conn_qp(cb[i], j, srv_name);
    }

    printf("main: Master published all QPs on port %d\n", ib_port_index);
  }

  hrd_red_printf("main: Master published all QPs. Waiting for clients..\n");

  for (i = 0; i < NUM_CLIENTS; i++) {
    char clt_conn_qp_name[HRD_QP_NAME_SIZE];
    sprintf(clt_conn_qp_name, "client-conn-%d", i);
    printf("main: Master waiting for client %s\n", clt_conn_qp_name);

    struct hrd_qp_attr* clt_qp = NULL;
    while (clt_qp == NULL) {
      clt_qp = hrd_get_published_qp(clt_conn_qp_name);
      if (clt_qp == NULL) {
        usleep(200000);
      }
    }

    printf("main: Master found client %d of %d clients. Connecting..\n", i,
           NUM_CLIENTS);

    /* Calculate the control block and QP to use for this client */
    int cb_i = i % num_server_ports;
    int qp_i = i;

    hrd_connect_qp(cb[cb_i], qp_i, clt_qp);

    char mstr_qp_name[HRD_QP_NAME_SIZE];
    sprintf(mstr_qp_name, "master-%d-%d", cb_i, qp_i);
    hrd_publish_ready(mstr_qp_name);
  }

  /*
   * Wait until the sun rises in the west and sets in the east. Until the
   * rivers run dry, and the mountains blow in the wind like leaves...
   */
  printf("main: Master sleeping\n");
  sleep(1000000);
  return NULL;
}
