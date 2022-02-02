#!/bin/bash 

TEST_OBJS="bin/ceph_test_objectstore"

Help()
{
   # Display Help and exit
   echo "Run ObjectStore tests on virtual or real SMR device"
   echo
   echo "Syntax: run_smr_tests [-s|d|f|h]"
   echo "options:"
   echo "s     Toggle SMR mode (on=1|off=0) Default=0"
   echo "d     In SMR mode, path required to SMR device mountpoint"
   echo "f     Gtest filter string, default=*/2"
   echo "h     Print this Help"
   echo

   exit
}

BuildVirtDevice()
{
	# Builds virtual device to run tests
	# taken from original script
	before_creation=$(mktemp)
	lsscsi > $before_creation

	echo "cd /backstores/user:zbc
	create name=zbc0 size=20G cfgstring=model-HM/zsize-256/conv-10@zbc0.raw
	/loopback create
	cd /loopback
	create naa.50014055e5f25aa0
	cd naa.50014055e5f25aa0/luns
	create /backstores/user:zbc/zbc0 0
	" | sudo targetcli

	sleep 1 #if too fast device does not show up
	after_creation=$(mktemp)
	lsscsi > $after_creation
	if [[ $(diff $before_creation $after_creation | wc -l ) != 2 ]]
	then
	    echo New zbc device not created
	    false
	fi

	function cleanup() {
	    echo "cd /loopback
	delete naa.50014055e5f25aa0
	cd /backstores/user:zbc
	delete zbc0" | sudo targetcli
	    sudo rm -f zbc0.raw
	    rm -f $before_creation $after_creation

	    sudo targetcli exit
	}

	trap cleanup EXIT

	DEV=$(diff $before_creation $after_creation |grep zbc |sed "s@.* /@/@")
	sudo chmod 666 $DEV

}

# Parse cmd line args
while getopts s:d:f:h flag
do
    case "${flag}" in
	h) Help
	   ;;
        s) smr=${OPTARG}
	   if [[ $smr -eq 1 ]] || [[ $smr -eq 0 ]]
	   then
		   echo ''
	   else 
		   Help
	   fi
	   ;;
        d) device_path=${OPTARG};;
        f) gfilterstr=${OPTARG};;
	\?)echo "Run $0 with -h for usage"
           exit;;
    esac
done

# Set default filter
if [[ "$gfilterstr" == "" ]]
then
	gfilterstr="*/2"
fi

# Run tests
if [[ $smr -eq 1 ]]	
then
	if [[ -n "$device_path" ]]
	then
		sudo ./${TEST_OBJS} --smr \
	       	--bluestore-block-path=$device_path \
			--gtest_filter=$gfilterstr 
	else
		echo "Device path missing."
		Help
	fi
else
	read -p "Do you want to build a virtual device to run tests ? [Y/n]: " resp
	if [[ $resp == "Y" ]]; then
		BuildVirtDevice
		sudo ./${TEST_OBJS} \
		    --bluestore-block-path $DEV \
		    --gtest_filter=$gfilterstr \
		    $*
	fi

fi

