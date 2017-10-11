# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

export HRD_REGISTRY_IP="fawn-pluto0"

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
	numactl --cpunodebind=0 --membind=0 ./atomics-sequencer \
	--num_threads $num_threads \
	--base_port_index 0 \
	--num_server_ports 2 \
	--num_client_ports 2 \
	--postlist 16 \
	--is_client 1 \
	--machine_id $1
