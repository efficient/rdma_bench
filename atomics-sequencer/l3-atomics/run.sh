echo "Removing hugepages"
sudo ipcrm -M 24

sudo numactl --cpunodebind=0 --membind=0 ./main
