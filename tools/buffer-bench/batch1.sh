#!/usr/bin/env bash
cd /e/dd-bench-work
S6=/e/dd-bench-work/src_6M.flv; S8=/e/dd-bench-work/src_8M.flv
CT='C:\dd-bench-tmp'; ET='E:\dd-bench-tmp'; O=/e/dd-bench-out
P=19410
go() { bash run.sh "$@" & sleep 1; }
# disk mode, spill on C:
go cd6_10  $S6 "$CT" $O "0:10,660:300" 60 1500 0 $((P+0))
go cd6_120 $S6 "$CT" $O "0:120"        60 1500 0 $((P+1))
go cd6_600 $S6 "$CT" $O "0:600"        60 1500 0 $((P+2))
go cd8_10  $S8 "$CT" $O "0:10"         60 1500 0 $((P+3))
go cd8_120 $S8 "$CT" $O "0:120"        60 1500 0 $((P+4))
go cd8_600 $S8 "$CT" $O "0:600"        60 1500 0 $((P+5))
# RAM only, keep 60
go r60_6_10  $S6 "$ET" $O "0:10,660:300" 60 1500 -1 $((P+6))
go r60_6_120 $S6 "$ET" $O "0:120"        60 1500 -1 $((P+7))
go r60_6_600 $S6 "$ET" $O "0:600"        60 1500 -1 $((P+8))
go r60_8_10  $S8 "$ET" $O "0:10"         60 1500 -1 $((P+9))
go r60_8_120 $S8 "$ET" $O "0:120"        60 1500 -1 $((P+10))
go r60_8_600 $S8 "$ET" $O "0:600"        60 1500 -1 $((P+11))
# RAM only, keep = delay
go rd_6_10  $S6 "$ET" $O "0:10,660:300" 10  1500 -1 $((P+12))
go rd_6_120 $S6 "$ET" $O "0:120"        120 1500 -1 $((P+13))
go rd_8_10  $S8 "$ET" $O "0:10"         10  1500 -1 $((P+14))
go rd_8_120 $S8 "$ET" $O "0:120"        120 1500 -1 $((P+15))
wait
echo BATCH1 DONE
