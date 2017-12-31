#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="xia-wimpy"

drop_shm

if [ "$#" -gt 2 -o "$#" -eq 0 ]; then
  blue "Illegal number of parameters"
  blue "Params: <machine_number>, or <machine_number> gdb"
	exit
fi

# The 0th machine hosts the QP registry
if [ "$1" -eq 0 ]; then
	blue "Machine 0: Resetting QP registry"
	sudo killall memcached 1>/dev/null 2>/dev/null
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
	sleep 1
fi

blue "Machine $1: Starting worker threads"

flags="
  --machine_id $1 \
  --base_port_index 0 \
  --num_ports 2 \
  --use_uc 0 \
  --do_read 1
"

rm -f tput-out/*

# Check for non-gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 ../build/rc-swarm $flags
fi

# Check for gdb mode
if [ "$#" -eq 2 ]; then
  sudo -E gdb -ex run --args ../build/rc-swarm $flags
fi
