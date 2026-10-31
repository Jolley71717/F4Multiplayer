# smooth.awk -v who=<p2|me> -v label=<text> <trace.csv>: how evenly one actor moved, frame by frame. For each sample, the
# step's speed (distance over time from the sample before) against the speed over the 300 ms around it: a stand-in drawn
# evenly steps at about that speed every frame; one that stands a frame and then leaps shows as a stall and a leap. Only
# samples moving faster than 100 u/s over that window count. Repeated stamps (no new engine update) are dropped.
# Used by trace.sh analyse; best on a trace taken every frame (TRACE_HZ=240).
BEGIN { FS="," }
FNR==1 { next }
$2==who { if (n && $1 <= t[n]) next; n++; t[n]=$1; x[n]=$3; y[n]=$4 }
END {
	if (n < 20) { printf "%s: not enough samples (%d)\n", label, n; exit }
	for (i=2; i<=n; i++) { dt[i]=t[i]-t[i-1]; st[i]=sqrt((x[i]-x[i-1])^2+(y[i]-y[i-1])^2) }
	# steps lo..hi are those ending within 150 ms of t[i]
	lo=2; hi=1; dsum=0; tsum=0; cnt=0; stalls=0; leaps=0; jsum=0; jn=0; ftsum=0
	for (i=2; i<=n; i++) {
		while (hi < n && t[hi+1] <= t[i] + 150) { hi++; dsum+=st[hi]; tsum+=dt[hi] }
		while (lo <= hi && t[lo] < t[i] - 150) { dsum-=st[lo]; tsum-=dt[lo]; lo++ }
		if (dt[i] < 2 || dt[i] > 200 || tsum <= 0) continue
		avg=dsum/tsum*1000; v=st[i]/dt[i]*1000
		if (avg < 100) continue
		r=(v-avg)/avg; if (r<0) r=-r
		cnt++; rel[cnt]=r; ftsum+=dt[i]
		if (v < 0.3*avg) stalls++
		if (v > 1.7*avg) leaps++
		if (i < n && dt[i+1] >= 2 && dt[i+1] <= 200) { f=dt[i]/(dt[i]+dt[i+1]); px=x[i-1]+(x[i+1]-x[i-1])*f; py=y[i-1]+(y[i+1]-y[i-1])*f; jsum+=sqrt((x[i]-px)^2+(y[i]-py)^2); jn++ }
	}
	if (cnt < 10) { printf "%s: too few moving samples (%d)\n", label, cnt; exit }
	asort(rel); s=0; for (i=1;i<=cnt;i++) s+=rel[i]
	p95=rel[int(cnt*0.95)]; p99=rel[int(cnt*0.99)]
	printf "%s: %d moving frames (%.1f ms apart) | step speed off its 300 ms average: mean %.1f%%, p95 %.0f%%, p99 %.0f%% | stalls %d (%.1f%%), leaps %d (%.1f%%) | off the line between neighbours: mean %.2f u\n", \
		label, cnt, ftsum/cnt, s/cnt*100, p95*100, p99*100, stalls, stalls*100/cnt, leaps, leaps*100/cnt, (jn ? jsum/jn : 0)
}
