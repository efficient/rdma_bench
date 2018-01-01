#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="10.100.3.13"

drop_shm
exe="../build/rc-swarm"
chmod +x $exe

if [ "$#" -gt 2 -o "$#" -eq 0 ]; then
  blue "Illegal number of parameters"
  blue "Params: <machine_number>, or <machine_number> gdb"
	exit
fi

# The 0th machine hosts the QP registry
if [ "$1" -eq 0 ]; then
	blue "Machine 0: Resetting QP registry"
	sudo killall memcached 1>/dev/null 2>/dev/null

  # Spawn memcached, but wait for it to start
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
  while ! nc -z localhost 11211; do sleep .1; done
  echo "Machine 0: memcached server is open for business on port 11211"
fi

blue "Machine $1: Starting worker threads"
rm -f tput-out/*

flags="--machine_id $1 $(cat config)"
# Check for non-gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 $exe $flags
fi

# Check for gdb mode
if [ "$#" -eq 2 ]; then
  sudo -E gdb -ex run --args $exe $flags
fi
