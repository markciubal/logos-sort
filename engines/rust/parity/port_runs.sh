#!/bin/bash
# The port's record: route equivalence against C over the whole grid, then its speed against C. Logs go to parity/logs-port/.
#   bash parity/port_runs.sh
cd "$(dirname "$0")"
L=logs-port; mkdir -p "$L"
bash routes.sh 2,3,17,100,2047,2048,4096,5000,20000,65536,70000,131072,250000,500000,1000003,2000003,4194304,8388608 1 > "$L/routes-1t.log" 2>&1
bash routes.sh 100000,200000,300000,650000,1000003,2000003,8388608 2,3,4,6,8,14 > "$L/routes-mt.log" 2>&1
bash parity.sh 8388608 12 14 > "$L/speed-board-8m.log" 2>&1
bash parity.sh 8388608 10 14 extra > "$L/speed-extra-8m.log" 2>&1
bash parity.sh 1048576 16 14 > "$L/speed-board-1m.log" 2>&1
tail -n 1 "$L"/routes-*.log
grep -- "->" "$L"/speed-*.log
