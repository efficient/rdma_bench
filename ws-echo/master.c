#include "hrd.h"
#include "main.h"

/* Random local_hids for master */
#define MASTER_P0_ID -22
#define MASTER_P1_ID -23

void* run_master(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;

  struct hrd_ctrl_blk* cb[2]; /* 1 control block per port */

  for (i = 0; i < 2; i++) {
    cb[i] = hrd_ctrl_blk_init(MASTER_P0_ID, (params.dual_port == 0 ? 0 : i % 2),
                              0, /* port index, numa node*/
                              NUM_CLIENTS / 2, USE_UC, /* conn qps, uc */
                              NULL, RR_SIZE,
                              MASTER_SHM_KEY + i, /* prealloc buf, size, key */
                              0, 0, -1); /* #dgram qps, buf size, shm key */

    /* Zero out the request buffers */
    memset((void*)cb[i]->conn_buf, 0, RR_SIZE);
  }

  for (i = 0; i < NUM_CLIENTS; i++) {
    char mstr_qp_name[HRD_QP_NAME_SIZE];
    sprintf(mstr_qp_name, "master-%d", i);

    hrd_publish_conn_qp(cb[i % 2], i / 2, mstr_qp_name);
  }

  for (i = 0; i < NUM_CLIENTS; i++) {
    char clt_conn_qp_name[HRD_QP_NAME_SIZE];
    sprintf(clt_conn_qp_name, "client-conn-%d", i);

    struct hrd_qp_attr* clt_qp = NULL;
    while (clt_qp == NULL) {
      clt_qp = hrd_get_published_qp(clt_conn_qp_name);
      if (clt_qp == NULL) {
        usleep(200000);
      }
    }

    printf("main: Master found client %s! Connecting..\n", clt_conn_qp_name);
    hrd_connect_qp(cb[i % 2], i / 2, clt_qp);

    char mstr_qp_name[HRD_QP_NAME_SIZE];
    sprintf(mstr_qp_name, "master-%d", i);
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
