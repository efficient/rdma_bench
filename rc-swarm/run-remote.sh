# Similar to run-servers.sh, but used for remote runs by run-all.sh
# A function to echo in blue color
function blue() {
	echo "$1"
}

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-remote.sh <machine_number>"
	exit
fi

export HRD_REGISTRY_IP="10.113.1.47"
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
	sudo pkill memcached 1>/dev/null 2>/dev/null
	memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
	sleep 1
fi

blue "Machine $1: Starting worker threads"

rm -f tput-out/*
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--machine-id $1 \
	--base-port-index 0 \
	--num-ports 2 \
	--do-read 1 1>tput-out/err-machine-$1 2>tput-out/err-machine-$1 &

# Benchmark duration
sleep 120

blue "Machine $1: Done! Killing main until it dies"
while true; do
	sudo pkill main 1>/dev/null 2>/dev/null
	main_ps_lines=`ps -afx | grep main | wc -l`
	if [ $main_ps_lines = "1" ]; then
		blue "Machine $1: main is dead :)"
		break
	fi

	# Try after a while
	sleep 1
done
