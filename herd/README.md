# HERD
An improved implementation of the HERD key-value store.

## Running the benchmark
Refer to the top-level [README](https://github.com/efficient/rdma_bench/blob/master/README.md) for general requirements and instructions to run `rdma_bench` benchmarks. Quick start:

1. At the server machine: `./run-servers.sh`
2. At machine `i` of `NUM_WORKERS / num_threads` client machines:
`./run-machine.sh i`

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

For example, to run 16 worker threads on a server with IP address `server_ip`, and 20 client threads spread over 2 client machines, use the following settings:
  * Set `HRD_REGISTRY_IP = server_ip` in `run-servers.sh`
  * Set `HRD_REGISTRY_IP = server_ip` in each `run-machine.sh` file
  * `NUM_WORKERS = 16`, `NUM_CLIENTS = 30`, `num_threads = 10`
  * `./run-servers.sh` on the server machine
  * `./run-machine.sh 0` on client machine 0
  * `./run-machine.sh 1` on client machine 1

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
