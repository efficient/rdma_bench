# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-machine.sh <machine_number>"
	exit
fi

export HRD_REGISTRY_IP="128.110.96.101"
export MLX5_SINGLE_THREADED=1

blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

num_threads=4			# Threads per client machine
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Running $num_threads client threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --physcpubind=0,1,2,3,4,5,6,7 --membind=0 ./main \
	--num-threads $num_threads \
	--dual-port 0 \
	--use-uc 0 \
	--is-client 1 \
	--machine-id $1 \
	--size 4 \
	--postlist 1 \
	--do-read 1
