#!/bin/bash
set -o pipefail
[ -z "$1" ] && niter=100000 || niter=$1
[ -z "$2" ] && flags=t || flags=$2

#automated tests:
maxCpu=$(getconf _NPROCESSORS_ONLN)
for (( t = 0; t <= $maxCpu; t++ )); do
	for mainPrio in 0 90 99; do
		args="$niter $t $mainPrio $flags"
		printf "mutex-test $args : "
		./mutex-test $args 2>&1 | cat > /dev/null# | cat > log-$t-$mainPrio #cat > /dev/null
		ERR=$?
		[ 0 -eq $ERR ] && echo Success || echo Failed $(echo -e "obase=2\n$ERR" | bc)
		sleep 0.4
	done
done

