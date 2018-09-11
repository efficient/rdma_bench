#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="fawn-pluto0"

drop_shm

# Check number of arguments
if [ "$#" -gt 1 ]; then
  blue "Illegal number of arguments."
  blue "Usage: ./run-servers.sh, or ./run-servers.sh gdb"
	exit
fi

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting server thread"

flags="
	--base_port_index 0 \
	--num_server_ports 2 \
  --is_client 0
"

# Check for non-gdb mode
if [ "$#" -eq 0 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 ../build/atomics-sequencer $flags
fi

# Check for gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E gdb -ex run --args ../build/atomics-sequencer $flags
fi
