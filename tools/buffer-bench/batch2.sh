#!/usr/bin/env bash
cd /e/dd-bench-work
S6=/e/dd-bench-work/src_6M.flv; S8=/e/dd-bench-work/src_8M.flv
ET='E:\dd-bench-tmp'; O=/c/dd-bench-out
P=19430
go() { bash run.sh "$@" & sleep 1; }
go ed6_10  $S6 "$ET" $O "0:10,660:300" 60 1500 0 $((P+0))
go ed6_120 $S6 "$ET" $O "0:120"        60 1500 0 $((P+1))
go ed6_600 $S6 "$ET" $O "0:600"        60 1500 0 $((P+2))
go ed8_10  $S8 "$ET" $O "0:10"         60 1500 0 $((P+3))
go ed8_120 $S8 "$ET" $O "0:120"        60 1500 0 $((P+4))
go ed8_600 $S8 "$ET" $O "0:600"        60 1500 0 $((P+5))
wait
echo BATCH2 DONE
