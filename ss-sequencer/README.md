# Spec-S0
A 64-bit sequencer that uses UD transport only. The 32-bit Immediate header is
used to avoid payload DMAs and, on Mellanox NICs, to fit the SEND WQE in one
cache line.

## Important parameters
These parameters are defined in `main.h`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `NUM_SERVER_THREADS`: Number of worker threads at the server machine
  * `MAX_NUM_CLIENTS`: Maximum number of client threads in the cluster
  * `num_threads`: Number of client threads at each client machine
2. RDMA optimizations
  * `postlist`: Maximum number of responses sent by the server using one Doorbell
  * `DISABLE_SERVER_POSTLIST`: Disable Doorbell batching
  * `NUM_UD_QPS`: Number of UD QPs used by each server thread for responses
  * `UNSIG_BATCH`: One response in every `UNSIG_BATCH` responses is signaled

## Running the benchmark

1. At the server machine: `./run-servers.sh`
2. At client machine `i`: `./run-machine.sh i`
  * There can be up to `MAX_NUM_CLIENTS / num_threads` client machines

## Connection logic
 * Client `C` sends its requests to server `S = C % NUM_SERVER_THREADS`. In
   dual-port mode, it creates its control block on port `S % 2`.
 * The number of clients that send requests to a server thread is at most
   `Cm = ceil(MAX_NUM_CLIENTS / NUM_SERVER_THREADS)`. Each client keeps
   `postlist` requests outstanding. Therefore, the RECV queue at each server
   must contain at least `Cm * postlist` RECVs.
