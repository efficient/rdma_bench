#include "mica.h"
#include "hrd.h"

int is_power_of_2(int x) {
  return (x == 1 || x == 2 || x == 4 || x == 8 || x == 16 || x == 32 ||
          x == 64 || x == 128 || x == 256 || x == 512 || x == 1024 ||
          x == 2048 || x == 4096 || x == 8192 || x == 16384 || x == 32768 ||
          x == 65536 || x == 131072 || x == 262144 || x == 524288 ||
          x == 1048576 || x == 2097152 || x == 4194304 || x == 8388608 ||
          x == 16777216 || x == 33554432 || x == 67108864 || x == 134217728 ||
          x == 268435456 || x == 536870912 || x == 1073741824);
}

void mica_init(struct mica_kv* kv, int instance_id, int node_id, int num_bkts,
               int log_cap) {
  int i, j;

  /* Verify struct sizes */
  assert(sizeof(struct mica_slot) == 8);
  assert(sizeof(struct mica_key) == 16);
  assert(sizeof(struct mica_op) % 64 == 0);

  assert(kv != NULL);
  assert(node_id == 0 || node_id == 1);

  /* 16 million buckets = a 1 GB index */
  assert(is_power_of_2(num_bkts) == 1 && num_bkts <= M_16);
  assert(log_cap > 0 && log_cap <= M_1024 && log_cap % M_2 == 0 &&
         is_power_of_2(log_cap));

  assert(MICA_LOG_BITS >= 24); /* Minimum log size = 16 MB */

  hrd_red_printf(
      "mica: Initializing MICA instance %d.\n"
      "NUMA node = %d, buckets = %d (size = %u B), log capacity = %d B.\n",
      instance_id, node_id, num_bkts, num_bkts * sizeof(struct mica_bkt),
      log_cap);

  if (MICA_DEBUG != 0) {
    printf("mica: Debug mode is ON! This might reduce performance.\n");
    sleep(2);
  }

  /* Initialize metadata and stats */
  kv->instance_id = instance_id;
  kv->node_id = node_id;

  kv->num_bkts = num_bkts;
  kv->bkt_mask = num_bkts - 1; /* num_bkts is power of 2 */

  kv->log_cap = log_cap;
  kv->log_mask = log_cap - 1; /* log_cap is a power of 2 */
  kv->log_head = 0;

  kv->num_get_op = 0;
  kv->num_get_fail = 0;
  kv->num_put_op = 0;
  kv->num_index_evictions = 0;

  /* Alloc index and initialize all entries to invalid */
  printf("mica: Allocting hash table index for instance %d\n", instance_id);
  int ht_index_key = MICA_INDEX_SHM_KEY + instance_id;
  kv->ht_index = (struct mica_bkt*)hrd_malloc_socket(
      ht_index_key, num_bkts * sizeof(struct mica_bkt), node_id);

  for (i = 0; i < num_bkts; i++) {
    for (j = 0; j < 8; j++) {
      kv->ht_index[i].slots[j].in_use = 0;
    }
  }

  /* Alloc log */
  printf("mica: Allocting hash table log for instance %d\n", instance_id);
  int ht_log_key = MICA_LOG_SHM_KEY + instance_id;
  kv->ht_log = (uint8_t*)hrd_malloc_socket(ht_log_key, log_cap, node_id);
}

void mica_insert_one(struct mica_kv* kv, struct mica_op* op,
                     struct mica_resp* resp) {
#if MICA_DEBUG == 1
  assert(kv != NULL);
  assert(op != NULL);
  assert(op->opcode == MICA_OP_PUT);
  assert(op->val_len > 0 && op->val_len <= MICA_MAX_VALUE);
  assert(resp != NULL);
#endif

  int i;
  unsigned int bkt = op->key.bkt & kv->bkt_mask;
  struct mica_bkt* bkt_ptr = &kv->ht_index[bkt];
  unsigned int tag = op->key.tag;

#if MICA_DEBUG == 2
  mica_print_op(op);
#endif

  kv->num_put_op++;

  /* Find a slot to use for this key. If there is a slot with the same
   * tag as ours, we are sure to find it because the used slots are at
   * the beginning of the 8-slot array. */
  int slot_to_use = -1;
  for (i = 0; i < 8; i++) {
    if (bkt_ptr->slots[i].tag == tag || bkt_ptr->slots[i].in_use == 0) {
      slot_to_use = i;
    }
  }

  /* If no slot found, choose one to evict */
  if (slot_to_use == -1) {
    slot_to_use = tag & 7; /* tag is ~ randomly distributed */
    kv->num_index_evictions++;
  }

  /* Encode the empty slot */
  bkt_ptr->slots[slot_to_use].in_use = 1;
  bkt_ptr->slots[slot_to_use].offset = kv->log_head; /* Virtual head */
  bkt_ptr->slots[slot_to_use].tag = tag;

  /* Paste the key-value into the log */
  uint8_t* log_ptr = &kv->ht_log[kv->log_head & kv->log_mask];

  /* Data copied: key, opcode, val_len, value */
  int len_to_copy =
      sizeof(struct mica_key) + sizeof(uint8_t) + sizeof(uint8_t) + op->val_len;

  /* Ensure that we don't wrap around in the *virtual* log space even
   * after 8-byte alignment below.*/
  assert((1ULL << MICA_LOG_BITS) - kv->log_head > len_to_copy + 8);

  memcpy(log_ptr, op, len_to_copy);
  kv->log_head += len_to_copy;

  /* Ensure that the key field of each log entry is 8-byte aligned. This
   * makes subsequent comparisons during GETs faster. */
  kv->log_head = (kv->log_head + 7) & ~7;

  /* If we're close to overflowing in the physical log, wrap around to
   * the beginning, but go forward in the virtual log. */
  if (unlikely(kv->log_cap - kv->log_head <= MICA_MAX_VALUE + 32)) {
    kv->log_head = (kv->log_head + kv->log_cap) & ~kv->log_mask;
    hrd_red_printf("mica: Instance %d wrapping around. Wraps = %llu\n",
                   kv->instance_id, kv->log_head / kv->log_cap);
  }
}

/*
 * Process a batch of operations.
 * The caller expects @mica_resp to have valid @type, @val_len, and @val_ptr
 * fields. For failed GETs and all PUTs, @val_len should be 0 so that HERD can
 * send a 0-byte response. In these cases, the value of @val_ptr doesn't matter.
 */
void mica_batch_op(struct mica_kv* kv, int n, struct mica_op** op,
                   struct mica_resp* resp) {
  int I, j; /* I is batch index */
#if MICA_DEBUG == 1
  assert(kv != NULL);
  assert(op != NULL);
  assert(n > 0 && n <= MICA_MAX_BATCH_SIZE);
  assert(resp != NULL);
#endif

#if MICA_DEBUG == 2
  for (I = 0; I < n; I++) {
    mica_print_op(op[I]);
  }
#endif

  unsigned int bkt[MICA_MAX_BATCH_SIZE];
  struct mica_bkt* bkt_ptr[MICA_MAX_BATCH_SIZE];
  unsigned int tag[MICA_MAX_BATCH_SIZE];
  int key_in_store[MICA_MAX_BATCH_SIZE]; /* Is this key in the datastore? */
  struct mica_op* kv_ptr[MICA_MAX_BATCH_SIZE]; /* Ptr to KV item in log */

  /*
   * We first lookup the key in the datastore. The first two @I loops work
   * for both GETs and PUTs.
   */
  for (I = 0; I < n; I++) {
    bkt[I] = op[I]->key.bkt & kv->bkt_mask;
    bkt_ptr[I] = &kv->ht_index[bkt[I]];
    __builtin_prefetch(bkt_ptr[I], 0, 0);
    tag[I] = op[I]->key.tag;

    key_in_store[I] = 0;
    kv_ptr[I] = NULL;
  }

  for (I = 0; I < n; I++) {
    for (j = 0; j < 8; j++) {
      if (bkt_ptr[I]->slots[j].in_use == 1 &&
          bkt_ptr[I]->slots[j].tag == tag[I]) {
        uint64_t log_offset = bkt_ptr[I]->slots[j].offset & kv->log_mask;

        /*
         * We can interpret the log entry as mica_op, even though it
         * may not contain the full MICA_MAX_VALUE value.
         */
        kv_ptr[I] = (struct mica_op*)&kv->ht_log[log_offset];

        /* Small values (1--64 bytes) can span 2 cache lines */
        __builtin_prefetch(kv_ptr[I], 0, 0);
        __builtin_prefetch((uint8_t*)kv_ptr[I] + 64, 0, 0);

        /* Detect if the head has wrapped around for this index entry */
        if (kv->log_head - bkt_ptr[I]->slots[j].offset >= kv->log_cap) {
          kv_ptr[I] = NULL; /* If so, we mark it "not found" */
        }

        break;
      }
    }
  }

  for (I = 0; I < n; I++) {
    if (kv_ptr[I] != NULL) {
      /* We had a tag match earlier. Now compare log entry. */
      long long* key_ptr_log = (long long*)kv_ptr[I];
      long long* key_ptr_req = (long long*)op[I];

      if (key_ptr_log[0] == key_ptr_req[0] &&
          key_ptr_log[1] == key_ptr_req[1]) {
        key_in_store[I] = 1;

        if (op[I]->opcode == MICA_OP_GET) {
          kv->num_get_op++;

          resp[I].type = MICA_RESP_GET_SUCCESS;
          resp[I].val_len = kv_ptr[I]->val_len;
          resp[I].val_ptr = kv_ptr[I]->value;
        } else {
          kv->num_put_op++;
          /* Update the value in the log */
          assert(op[I]->val_len == kv_ptr[I]->val_len);
          memcpy(kv_ptr[I]->value, op[I]->value, kv_ptr[I]->val_len);

          resp[I].type = MICA_RESP_PUT_SUCCESS;
          resp[I].val_len = 0;
          resp[I].val_ptr = NULL;
        }
      }
    }

    if (key_in_store[I] == 0) {
      /* We get here if either tag or log key match failed */
      if (op[I]->opcode == MICA_OP_GET) {
        kv->num_get_fail++;

        resp[I].type = MICA_RESP_GET_FAIL;
        resp[I].val_len = 0;
        resp[I].val_ptr = NULL;
      } else {
        /*
         * If datastore lookup failed for PUT, it's an INSERT. INSERTs
         * currently happen on a slow non-batched path.
         */
        mica_insert_one(kv, op[I], &resp[I]);

        resp[I].type = MICA_RESP_PUT_SUCCESS;
        resp[I].val_len = 0;
        resp[I].val_ptr = NULL;
      }
    }
  }
}

void mica_print_bucket(struct mica_kv* kv, int bkt_idx) {
  assert(kv != NULL);
  assert(bkt_idx > 0 && bkt_idx < kv->num_bkts);

  int i;
  struct mica_bkt* bkt = &kv->ht_index[bkt_idx];
  printf("mica: Bucket %d:\n", bkt_idx);
  for (i = 0; i < 8; i++) {
    printf("\tSlot %d: in_use = %u, offset = {%lu, %u} tag = %u\n", i,
           bkt->slots[i].in_use, (uint64_t)bkt->slots[i].offset,
           (uint32_t)(bkt->slots[i].offset & kv->log_mask), bkt->slots[i].tag);
  }
}

void mica_print_op(struct mica_op* op) {
  assert(op != NULL);

  int i;
  long long* key = (long long*)&op->key;

  if (op->opcode == MICA_OP_GET) {
    printf("mica: GET for key %llx %llx\n", key[0], key[1]);
  }

  if (op->opcode == MICA_OP_PUT) {
    printf("mica: PUT for key %llx %llx. Value = ", key[0], key[1]);
    assert(op->val_len > 0 && op->val_len <= MICA_MAX_VALUE);
    for (i = 0; i < op->val_len; i++) {
      printf("%d ", op->value[i]);
    }
    printf("\n");
  }
}

/* A fast deterministic way to generate @n ~randomly distributed 16-byte keys */
uint128* mica_gen_keys(int n) {
  int i;
  assert(n > 0 && n <= M_1024 / sizeof(uint128));
  assert(sizeof(uint128) == 16);

  printf("mica: Generating %d keys\n", n);

  uint128* key_arr = malloc(n * sizeof(uint128));
  assert(key_arr != NULL);

  for (i = 0; i < n; i++) {
    key_arr[i] = CityHash128((char*)&i, 4);
  }

  return key_arr;
}

/*
 * Populate the KVS with @n keys, each with value length = @val_len.
 *  - The ith key is the CityHash128 value of i.
 *  - The bytes of the value inserted are all equal to the least significant
 *    byte of the key.
 */
void mica_populate_fixed_len(struct mica_kv* kv, int n, int val_len) {
  assert(kv != NULL);
  assert(n > 0);
  assert(val_len > 0 && val_len <= MICA_MAX_VALUE);

  /* This is needed for the eviction message below to make sense */
  assert(kv->num_put_op == 0 && kv->num_index_evictions == 0);

  int i;
  struct mica_op op;
  unsigned long long* op_key = (unsigned long long*)&op.key;
  struct mica_resp resp;

  /* Generate the keys to insert */
  uint128* key_arr = mica_gen_keys(n);

  for (i = 0; i < n; i++) {
    op_key[0] = key_arr[i].first;
    op_key[1] = key_arr[i].second;
    op.opcode = MICA_OP_PUT;

    op.val_len = val_len;
    uint8_t val = (uint8_t)(op_key[1] & 0xff);
    memset(op.value, val, val_len);

    mica_insert_one(kv, &op, &resp);
  }

  assert(kv->num_put_op == n);
  printf(
      "mica: Populated instance %d with %d keys, length = %d. "
      "Index eviction fraction = %.4f.\n",
      kv->instance_id, n, val_len,
      (double)kv->num_index_evictions / kv->num_put_op);
}
