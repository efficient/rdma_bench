# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

function node_name_from_id() {
	echo "10.113.1.$1"
}

node_list="47 48 49 50 51 52 46 65 66 91 92"

# Remove old tput-out directory contents. Doing it at node-1 clears the dir
# at all nodes bc NFS
rm -rf tput-out/*

# Check that there are no main processes running
blue "run-all: Checking status of main"
for i in $node_list; do
	node_name=`node_name_from_id $i`
	main_status=`ssh -oStrictHostKeyChecking=no $node_name \
		"cd ~/rdma_bench/rc-swarm; \
		./kill.sh status"`

	if [[ $main_status == *"YES"* ]]
	then
		echo "run-all: Error: $main_status. Retrying.";
	fi
done

blue "run-all: Check success."

# Kill old rc-swarm and start new ones at all servers
server_id=0
for i in $node_list; do
	blue "run-all: Starting server $server_id on node $i"
	node_name=`node_name_from_id $i`
	ssh -oStrictHostKeyChecking=no $node_name \
		"cd ~/rdma_bench/rc-swarm; \
		./run-remote.sh $server_id" &

	if [ "$server_id" -eq 0 ]; then
		blue "run-all: Giving 1 second for memcached to start"
		sleep 1
	fi
	server_id=`expr $server_id + 1`
done

# Wait for all the servers to finish
wait

# Copy the tput-out files from each server
blue "Removing old output and copying output from servers"
rm -f /tmp/machine-* 1>/dev/null 2>/dev/null
server_id=0
for i in $node_list; do
	node_name=`node_name_from_id $i`
	scp $node_name:~/rdma_bench/rc-swarm/tput-out/machine-$server_id \
		 /tmp/machine-$server_id 1>/dev/null 2>/dev/null &
	server_id=`expr $server_id + 1`
done

# Wait for all the scp's to finish
wait

# Record output to sweep folder
cat sweep.h >> sweep/temp_out
cat main.h | grep ALLSIG >> sweep/temp_out

echo "Performance for sweep params:"
cat sweep.h

server_id=0
for i in $node_list; do
	num_lines=`cat /tmp/machine-$server_id | wc -l`

	# In both correct and wrong cases, write something to sweep file
	if [ "$num_lines" -lt 7 ]; then
		# Print to screen
		possible_error=`cat /tmp/machine-$server_id | grep -i error`
		echo "Machine $server_id: Error? ($possible_error) or not enough data"
		# Save to sweep file
		server_tput=`echo "Machine $server_id: Error or not enough data"`
	else
		# Print to screen
		cat /tmp/machine-$server_id | tail -n+6 | cut -d' ' -f 5 | \
			awk -v server_id=$server_id '{sum += $1} \
			END{printf "Machine '"$server_id"' average: %.2f M/s\n", \
			(sum / NR) / 1000000}'

		# Save to sweep file
		server_tput=`cat /tmp/machine-$server_id | tail -n+6 | cut -d' ' -f 5 | \
			awk -v server_id=$server_id '{sum += $1} \
			END{printf "Machine '"$server_id"' average: %.2f M/s\n", \
			(sum / NR) / 1000000}'`
	fi

	echo $server_tput >> sweep/temp_out
	server_id=`expr $server_id + 1`
done

