function check_alive() {
	target=$1
	count=$( ping -i .2 -c 1 10.113.1.$1 | grep icmp* | wc -l )

	if [ $count -eq 0 ]
	then
		echo "$1 dead"
	else
		echo "$1 alive"
	fi
}

echo "Pinging and sleeping for 1 second"
touch checkalivetemp
for i in `seq 36 50`; do
	check_alive $i >> checkalivetemp &
done

sleep 1
cat checkalivetemp | sort
rm checkalivetemp
