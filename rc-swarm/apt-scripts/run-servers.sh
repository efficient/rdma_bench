# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-servers.sh <machine_number>"
	exit
fi

export HRD_REGISTRY_IP="128.110.96.178"
export MLX5_SINGLE_THREADED=1
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

# Remove SHM keys used by workers
for key in `seq 24 80`; do
	sudo ipcrm -M $key 1>/dev/null 2>/dev/null
done

: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

# The 0th machine hosts the QP registry
if [ "$1" -eq 0 ]; then
	blue "Machine $1: Resetting QP registry"
	sudo killall memcached 1>/dev/null 2>/dev/null
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
	sleep 1

	# Doing this at node-1 clears the dir at all nodes bc NFS
	blue "Clearing tput-out contents"
	rm -f tput-out/*
fi

blue "Machine $1: Starting worker threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--machine-id $1 \
	--base-port-index 0 \
	--num-ports 1 \
	--use-uc 0 \
	--do-read 1 &
