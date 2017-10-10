# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-machine.sh <machine_number>"
	exit
fi

blue "Removing shm keys 0--200"
for i in `seq 0 200`; do
	sudo ipcrm -M $i 1>/dev/null 2>/dev/null
done

: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Starting client"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--dual-port 1 \
	--run-time 10 \
	--use-uc 0 \
	--is-client 1 \
	--machine-id $1 \
