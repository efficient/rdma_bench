# HERD
An improved implementation of the HERD key-value store.

## Important parameters
These parameters are defined in `main.h`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `NUM_WORKERS`: Number of worker threads at the server machine
  * `NUM_CLIENTS`: Total number of client threads used
  * `num_threads`: Number of client threads at each client machine
2. Key-value store configuration
  * `HERD_NUM_KEYS`: Number of keys in each server threads's keyspace partition
  * `HERD_VALUE_SIZE`: Value size in bytes. Key size is fixed at 16 bytes
  * `update_percentage`: Percentage of update operations
3. RDMA optimizations
  * `postlist`: Maximum number of responses sent by the server using one Doorbell
  * `USE_POSTLIST`: Enable or disable Doorbell batching
  * `NUM_UD_QPS`: Number of UD QPs used by each server thread for responses
  * `UNSIG_BATCH`: One response in every `UNSIG_BATCH` responses is signaled

## Running the benchmark
1. At the server machine: `./run-servers.sh`
2. At machine `i` of `NUM_WORKERS / num_threads` client machines:
`./run-machine.sh i`

## Client connection logic
The technique discussed in the main README is used: Each client thread uses a
single control block and creates all its QPs on port index (using base
`base_port_index`) `clt_i % num_client_ports`. It connects all these QPs to QPs
on server port indexed `clt_i % num_server_ports` (using the server's
`base_port_index`). When responding to client `clt_i`, the server sends response
using its control block indexed `clt_i % num_server_ports`.

## TODOs
 * Currently, every worker creates a MICA instance and inserts all keys into it.
   The keys are 16-byte hashes of integers `{0, ..., HERD_NUM_KEYS - 1}`. Ideally,
   each instance should only insert keys that belong to its partition. Doing so
   should not affect performance, however.

## Performance on E5-2683-v3:
 * Commit `2dc97ff9e4aab4a5a0634c357d6946f484ed218d`
 * 5% UPDATEs
 * All numbers are million operations/sec
```
Cores  Batched  Non-batched
1      12.3		   6.69
2      23.2		   12.68
4      40.4     24.2
8      71.2     44
12     98.3     63.6			# Average batch = 14.14 packets
14     93.8     72.8			# Average batch = 8.9 packets
```
