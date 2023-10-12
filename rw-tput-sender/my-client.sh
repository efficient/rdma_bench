export HRD_REGISTRY_IP="10.0.0.44"
# lsync messes up permissions
executable="../build/rw-tput-sender"
chmod +x $executable

drop_shm
num_threads=1			# Threads per client machine
flags="\
  --num_threads $num_threads \
	--dual_port 0 \
	--use_uc 0 \
	--is_client 1 \
	--machine_id $1
"

flags="\
  --num_threads $num_threads \
	--dual_port 0 \
	--use_uc 0 \
	--is_client 1 \
	--machine_id $1
"
if [ "$#" -eq 1 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 $executable $flags
fi
