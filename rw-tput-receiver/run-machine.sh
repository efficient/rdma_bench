#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="fawn-pluto0"

drop_shm

num_threads=16			# Threads per client machine
blue "Running $num_threads client threads"

# Check number of arguments
if [ "$#" -gt 2 ]; then
  blue "Illegal number of arguments."
  blue "Usage: ./run-machine.sh <machine_id>, or ./run-machine.sh <machine_id> gdb"
	exit
fi

if [ "$#" -eq 0 ]; then
  blue "Illegal number of arguments."
  blue "Usage: ./run-machine.sh <machine_id>, or ./run-machine.sh <machine_id> gdb"
	exit
fi

flags="\
  --num_threads $num_threads \
	--dual_port 1 \
	--use_uc 1 \
	--is_client \
	--machine_id $1 \
	--size 32 \
	--postlist 4 \
  --do_read 0
"

# Check for non-gdb mode
if [ "$#" -eq 1 ]; then
  sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
    numactl --cpunodebind=0 --membind=0 ./main $flags
fi

# Check for gdb mode
if [ "$#" -eq 2 ]; then
  sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
    gdb -ex run --args ./main $flags
fi
