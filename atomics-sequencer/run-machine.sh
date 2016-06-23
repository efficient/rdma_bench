# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

export HRD_REGISTRY_IP="10.113.1.47"

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-machine.sh <machine_number>"
	exit
fi

blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

num_threads=8			# Threads per client machine
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Running $num_threads client threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_threads \
	--base-port-index 0 \
	--num-server-ports 2 \
	--num-client-ports 2 \
	--appnet 0 \
	--postlist 16 \
	--is-client 1 \
	--machine-id $1
