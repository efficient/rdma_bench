#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh

export HRD_REGISTRY_IP="10.100.3.13"
export MLX4_SINGLE_THREADED=1
export MLX5_SINGLE_THREADED=1
export MLX5_SHUT_UP_BF=0
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

exe="../build/rc-swarm"
chmod +x $exe

# Check arguments
if [ "$#" -gt 3 ] || [ "$#" -lt 2 ]; then
  blue "Illegal args. Usage: do.sh [process_id] [NUMA node] <gdb>"
	exit
fi

process_id=$1
numa_node=$2

# The 0th process starts up the QP registry
if [ "$process_id" -eq 0 ]; then
	blue "Process 0: Resetting QP registry"
	sudo pkill memcached 1>/dev/null 2>/dev/null

  # Spawn memcached, but wait for it to start
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
  while ! nc -z localhost 11211; do sleep .1; done
  echo "Process 0: memcached server is open for business on port 11211"
fi

blue "Process $process_id: Starting worker threads"
rm -f tput-out/*

flags="--process_id $process_id --numa_node $numa_node $(cat config)"

# Non-GDB mode
if [ "$#" -eq 2 ]; then
  blue "Launching process $process_id on NUMA node $numa_node"
  sudo -E numactl --cpunodebind=$numa_node --membind=$numa_node $exe $flags
fi

# GDB mode
if [ "$#" -eq 3 ]; then
  blue "Launching process $epid with GDB"
  sudo gdb -ex run --args $exe $flags
fi
