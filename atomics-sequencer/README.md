# RDMA atomics benchmark
This code is used for two benchmarks:

1. Raw atomics: Clients issue atomic operations on an array of 8-byte counters
   in the server's memory. Using a single counter converts the code into a
   benchmark for an atomics-based sequencer.
2. Emulating Dr-TM's key-value store: Clients emulate DrTM's key-value store
   operations using RDMA reads and atomics.

## Important parameters
These parameters are defined in `main.h`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `NUM_CLIENTS`: Total number of client threads used
  * `num_threads`: Number of client threads at each client machine
2. Raw atomics benchmark configuration
  * `BUF_SIZE`: Total memory (in bytes) used by the 8-byte counters
  * `USE_RANDOM`: If 0, all clients issue atomics on the same counter. This
    acts as a sequencer.
  * `STRIDE_SIZE`: Addresses used for atomic accesses are 0 modulo `STRIDE_SIZE`
3. DrTM key-value store configuration
  * `EMULATE_DRTM`: Configure the benchmark to emulate DrTM's key-value store
  * `UPDATEs`: Percentage of update operations for DrTM-KV
4. RDMA configuration
  * `HRD_CONNECT_IB_ATOMICS`: **Set to 1 if using an mlx5-based NIC (Connect-IB
    or newer)**

## Running the benchmark
1. At the server machine: `./run-servers.sh`
2. At machine `i` of `NUM_CLIENTS / num_threads` client machines:
   `./run-machine.sh i`

