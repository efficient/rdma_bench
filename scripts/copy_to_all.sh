if [ "$#" -ne 1 ]; then
    echo "Illegal number of parameters"
	echo "Usage: ./copy_to_all.sh <path_to_file_from_homedir>"
	exit
fi

local_server=47
for i in `36 37 38 39 44 46 47 48 49 50 51 52`; do
	if [ "$local_server" -ne "$i" ]; then
		echo "Copying $1 to anuj@10.113.1.$i"
		scp -r $1 anuj@10.113.1.$i:$1 &
		sleep .2
		killall scp
	fi
done
