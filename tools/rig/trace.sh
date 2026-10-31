#!/bin/bash
# Movement-sync harness. trace.sh run [seconds] drives the VM player along a path (run, turn, sneak,
# jump) while both games sample positions at 20 Hz (dev command "trace"), then fetches both files
# and compares the VM player's own path with the host's stand-in copy of it (and the reverse).
# trace.sh analyse [host.csv vm.csv [vmClockMs]] re-runs the comparison on fetched files. Outputs in stage/trace/.
# trace.sh clock resyncs the VM's clock and prints how far it is from the host's (no game needed).
#
# The two files are stamped with each machine's wall clock, so a VM clock that is off reads as lag and
# error: on 2026-10-17 the VM's w32time service was stopped, its clock drifted ~1.4 s, and every trace
# showed 200-400 u errors on old and new builds alike. So a run first resyncs the VM (w32time over ssh)
# and measures the offset; it refuses to trace when the clock is still more than MAX_CLOCK_MS off,
# measures again after the run (drift), and the fit takes the measured offset out of the VM's samples.
# The offset is saved in stage/trace/clock.txt next to the CSVs, where analyse finds it.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
K=$VM_KEY; H=$VM_SSH
O=$STAGE/trace; mkdir -p $O
HOSTCSV="$HOST_F4SE/F4Multiplayer_trace.csv"
MAX_CLOCK_MS=500   # a VM clock further off than this is not fitted at all
MAX_LAG_MS=1500    # the lag search's upper bound; a best lag there means the fit hit the limit
host() { bash "$RIG/dcfast.sh" host "$@" 2>/dev/null | tail -1; }
vm() { bash "$RIG/dcfast.sh" vm "$@" 2>/dev/null | tail -1; }

nowms() { local t=$EPOCHREALTIME; t=${t/./}; echo $((t / 1000)); }  # the host's clock in ms, without a fork

# clock_offset: prints the VM's clock minus the host's in ms. The VM prints its clock 20 times, 50 ms
# apart, and each line is stamped on arrival here; the largest (VM - arrival) is the offset less the
# shortest one-way delay over the virtual switch (a few ms). Fails if the VM didn't answer.
clock_offset() {
	local enc best= n=0 v t d
	enc=$(printf '%s' '1..20 | ForEach-Object { [Console]::Out.WriteLine([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()); [Console]::Out.Flush(); Start-Sleep -Milliseconds 50 }' | iconv -f utf-8 -t utf-16le | base64 -w0)
	while read -r v; do
		t=$(nowms); v=${v//[^0-9]/}; [ -n "$v" ] || continue
		d=$((v - t)); n=$((n + 1))
		if [ -z "$best" ] || [ $d -gt $best ]; then best=$d; fi
	done < <(timeout 30 ssh -i $K -o LogLevel=ERROR -o ConnectTimeout=8 $H "powershell -NoProfile -NonInteractive -EncodedCommand $enc" 2>/dev/null)
	[ -n "$best" ] && [ $n -ge 5 ] || return 1
	echo "$best"
}

# clock_resync: starts the VM's time service and resyncs it (ssh on the VM is elevated). Prints the result.
clock_resync() {
	timeout 40 ssh -i $K -o LogLevel=ERROR -o ConnectTimeout=8 $H 'Start-Service w32time -ErrorAction SilentlyContinue; "w32time: " + (Get-Service w32time).Status; w32tm /resync /force 2>&1 | Select-Object -Last 1' 2>&1 | tr -d '\r' | grep -v '^$' | sed 's/^/  /'
}

abs() { local v=$1; echo $((v < 0 ? -v : v)); }

# clock_check: resync, measure and judge the VM's clock. Sets CLOCK_MS; fails when unknown or too far off.
clock_check() {
	echo "VM clock: resyncing"
	clock_resync
	CLOCK_MS=$(clock_offset) || { echo "VM clock: could not measure it (ssh); not tracing"; return 1; }
	echo "VM clock: $CLOCK_MS ms from the host's (VM minus host, give or take the link delay of a few ms)"
	if [ $(abs $CLOCK_MS) -gt $MAX_CLOCK_MS ]; then
		echo "VM clock: more than $MAX_CLOCK_MS ms off after a resync: not tracing (fix the VM's time service first)"
		return 1
	fi
}

run() {
	secs=${1:-20}
	clock_check || return 1
	before=$CLOCK_MS
	# TRACE_HZ: samples a second (240 = every frame, for smooth); NETSIM / NETSIM_VM: netsim arguments for the host / the VM
	# (friends' states held back as over a worse connection, NetSim.h), turned off again after the run.
	[ -n "$NETSIM" ] && echo "host netsim: $(host "netsim $NETSIM")"
	[ -n "$NETSIM_VM" ] && echo "vm netsim: $(vm "netsim $NETSIM_VM")"
	bash "$RIG/dcfast.sh" both "trace start ${TRACE_HZ:-20}" >/dev/null
	vm focus >/dev/null; sleep 0.5
	# the path: run forward, turn right while running, sneak a bit, run back, jump
	bash "$RIG/dcfast.sh" both "key 0x57 3000" >/dev/null; sleep 3.2
	vm "mouse 600 0" >/dev/null; vm "key 0x57 2500" >/dev/null; sleep 2.8
	vm "key 0x11 100" >/dev/null; sleep 0.5; vm "key 0x57 2000" >/dev/null; sleep 2.3; vm "key 0x11 100" >/dev/null; sleep 0.5
	vm "mouse 1587 0" >/dev/null; sleep 0.3; bash "$RIG/dcfast.sh" both "key 0x57 3000" >/dev/null; sleep 3.3
	vm "key 0x20 80" >/dev/null; sleep 1.5
	rest=$((secs - 15)); [ $rest -gt 0 ] && sleep $rest
	echo "host: $(host 'trace stop')"; echo "vm:   $(vm 'trace stop')"
	[ -n "$NETSIM" ] && echo "host netsim after: $(host netsim)" && host "netsim off" >/dev/null
	[ -n "$NETSIM_VM" ] && echo "vm netsim after: $(vm netsim)" && vm "netsim off" >/dev/null
	cp "$HOSTCSV" $O/host.csv
	scp -q -i $K "$H:$VM_F4SE/F4Multiplayer_trace.csv" $O/vm.csv
	# Again after the run: a clock that moved during it is drift the fit can't take out.
	after=$(clock_offset) || after=$before
	echo "VM clock: $after ms after the run (drift $((after - before)) ms)"
	echo "$(( (before + after) / 2 )) $before $after" > $O/clock.txt
	analyse $O/host.csv $O/vm.csv $(( (before + after) / 2 ))
}

# compare <ownerCsv> <ownerWho> <viewerCsv> <viewerWho> <label> [ownerClockMs viewerClockMs]: the owner's own
# samples against the viewer's samples of the stand-in; the lag is the time shift that lines them up best.
# The clock arguments (that machine's clock minus the host's) are taken out of each file's timestamps.
compare() {
	awk -F, -v ownWho=$2 -v viewWho=$4 -v label="$5" -v ownOff=${6:-0} -v viewOff=${7:-0} -v maxLag=$MAX_LAG_MS '
	FNR==1 { file++; next }
	file==1 && $2==ownWho { n++; ot[n]=$1-ownOff; ox[n]=$3; oy[n]=$3*0+$4; oz[n]=$5 }
	file==2 && $2==viewWho { m++; vt[m]=$1-viewOff; vx[m]=$3; vy[m]=$4; vz[m]=$5 }
	END {
		if (n < 5 || m < 5) { printf "%s: not enough samples (owner %d, viewer %d)\n", label, n, m; exit }
		best=-1
		for (shift=0; shift<=maxLag; shift+=25) {
			sum=0; cnt=0; mx=0; j=1
			for (i=1; i<=m; i++) {
				t=vt[i]-shift   # where the owner was `shift` ms before the viewer drew it there
				# owner sample interpolated at t
				if (t < ot[1] || t > ot[n]) continue
				while (j<n && ot[j+1] < t) j++
				f=(ot[j+1]==ot[j]) ? 0 : (t-ot[j])/(ot[j+1]-ot[j])
				x=ox[j]+(ox[j+1]-ox[j])*f; y=oy[j]+(oy[j+1]-oy[j])*f
				d=sqrt((x-vx[i])^2+(y-vy[i])^2); sum+=d; cnt++; if (d>mx) mx=d
			}
			if (cnt==0) continue
			mean=sum/cnt
			if (shift==0) { m0=mean; x0=mx }
			if (best<0 || mean<best) { best=mean; bshift=shift; bmax=mx }
		}
		printf "%s: samples owner=%d viewer=%d | at no shift: mean err %.0f u, max %.0f u | best lag %d ms: mean err %.0f u, max %.0f u%s\n", label, n, m, m0, x0, bshift, best, bmax,
			(bshift >= maxLag ? "  CAPPED: the best lag is the search limit (" maxLag " ms): a clock or a stall, not a fit" : (bshift == 0 ? "  (best at 0 ms, the floor: check the clocks)" : ""))
	}' "$1" "$3"
}

svg() {  # svg <ownerCsv> <ownerWho> <viewerCsv> <viewerWho> <out.svg>
	awk -F, -v ownWho=$2 -v viewWho=$4 '
	FNR==1 { file++; next }
	file==1 && $2==ownWho { a[++n]=$3" "$4 }
	file==2 && $2==viewWho { b[++m]=$3" "$4 }
	{ if ((file==1 && $2==ownWho) || (file==2 && $2==viewWho)) { if (minx==""||$3<minx) minx=$3; if (maxx==""||$3>maxx) maxx=$3; if (miny==""||$4<miny) miny=$4; if (maxy==""||$4>maxy) maxy=$4 } }
	END {
		w=900; h=600; sx=(w-40)/(maxx-minx+1); sy=(h-40)/(maxy-miny+1); s=(sx<sy)?sx:sy
		printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" style=\"background:#111\">\n", w, h
		printf "<text x=\"10\" y=\"20\" fill=\"#9f9\" font-family=\"sans-serif\" font-size=\"14\">owner path (green)  stand-in as the other game drew it (orange)</text>\n"
		printf "<polyline fill=\"none\" stroke=\"#4f4\" stroke-width=\"2\" points=\""; for (i=1;i<=n;i++) { split(a[i],p," "); printf "%.1f,%.1f ", 20+(p[1]-minx)*s, h-20-(p[2]-miny)*s } printf "\"/>\n"
		printf "<polyline fill=\"none\" stroke=\"#f93\" stroke-width=\"2\" stroke-dasharray=\"4 3\" points=\""; for (i=1;i<=m;i++) { split(b[i],p," "); printf "%.1f,%.1f ", 20+(p[1]-minx)*s, h-20-(p[2]-miny)*s } printf "\"/>\n"
		print "</svg>"
	}' "$1" "$3" > "$5"
}

# npcs <host.csv> <vm.csv>: every NPC seen in both files, matched by nearest time: height and ground error.
# A mirrored NPC floating or sunk shows as dz well over an actor's height (about 120 units); flagged.
npcs() {
	awk -F, '
	FNR==1 { file++; next }
	substr($2,1,1)=="n" { id=$2; sub(/d$/,"",id); if (file==1) { a[id,++na[id]]=$1" "$3" "$4" "$5 } else { b[id,++nb[id]]=$1" "$3" "$4" "$5 } }
	END {
		for (id in na) { if (!(id in nb)) continue
			sumz=0; maxz=0; sumxy=0; maxxy=0; cnt=0
			for (i=1;i<=na[id];i++) { split(a[id,i],p," ")
				best=0; bd=1e18
				for (j=1;j<=nb[id];j++) { split(b[id,j],q," "); d=(p[1]-q[1]); if (d<0) d=-d; if (d<bd) { bd=d; best=j } }
				if (bd>150) continue; split(b[id,best],q," ")
				dz=p[4]-q[4]; if (dz<0) dz=-dz; dxy=sqrt((p[2]-q[2])^2+(p[3]-q[3])^2)
				sumz+=dz; sumxy+=dxy; cnt++; if (dz>maxz) maxz=dz; if (dxy>maxxy) maxxy=dxy }
			if (cnt>0) printf "%s samples=%d dz mean %.0f max %.0f | dxy mean %.0f max %.0f%s\n", id, cnt, sumz/cnt, maxz, sumxy/cnt, maxxy, (maxz>120 ? "  <-- FLOATING/SUNK" : "")
		}
	}' "$1" "$2" | sort
}

# analyse [host.csv vm.csv [vmClockMs]]: the VM's clock offset is the argument, else (for the files a run left
# in stage/trace) the one that run measured (clock.txt, written after vm.csv). An offset over MAX_CLOCK_MS is
# not fitted; with none known the files are fitted as they are, with a warning.
analyse() {
	h=${1:-$O/host.csv}; v=${2:-$O/vm.csv}; off=$3
	if [ -z "$off" ] && [ "$h" = "$O/host.csv" ] && [ "$v" = "$O/vm.csv" ] && [ -s $O/clock.txt ] && ! [ $O/vm.csv -nt $O/clock.txt ]; then
		read -r off _ < $O/clock.txt
	fi
	if [ -z "$off" ]; then
		echo "VM clock: unknown for these files (none given, no clock.txt from their run): lag and error include whatever the VM's clock was off"
		off=0
	elif [ $(abs $off) -gt $MAX_CLOCK_MS ]; then
		echo "VM clock: $off ms off, over $MAX_CLOCK_MS ms: not fitting (resync the VM with trace.sh clock and trace again)"
		return 1
	else
		echo "VM clock: taking $off ms out of the VM's samples"
	fi
	compare "$v" me "$h" p2 "VM player as the host sees them" $off 0
	compare "$h" me "$v" p1 "host player as the VM sees them" 0 $off
	# Frame by frame (smooth.awk): the stand-in as drawn, next to the owner's own path as their game drew it.
	awk -f $(dirname "$0")/smooth.awk -v who=p2 -v label="smooth: VM player as the host draws them" "$h"
	awk -f $(dirname "$0")/smooth.awk -v who=me -v label="smooth: VM player in their own game  " "$v"
	awk -f $(dirname "$0")/smooth.awk -v who=p1 -v label="smooth: host player as the VM draws them" "$v"
	awk -f $(dirname "$0")/smooth.awk -v who=me -v label="smooth: host player in their own game " "$h"
	# On the engine's frame time (builds that write fme rows): the player stamped with the frame the timer last timed (fme) or
	# the one before (qme), and each stand-in at the time it was placed at (fp).
	if grep -q ",fme," "$h"; then
		awk -f $(dirname "$0")/smooth.awk -v who=fp2 -v label="smooth: VM player as the host draws them, frame time" "$h"
		awk -f $(dirname "$0")/smooth.awk -v who=fme -v label="smooth: VM player in their own game, frame time" "$v"
		awk -f $(dirname "$0")/smooth.awk -v who=qme -v label="smooth: VM player in their own game, frame before" "$v"
		awk -f $(dirname "$0")/smooth.awk -v who=fp1 -v label="smooth: host player as the VM draws them, frame time" "$v"
		awk -f $(dirname "$0")/smooth.awk -v who=tp2 -v label="smooth: VM player as the host draws them, at the update's frame" "$h"
		awk -f $(dirname "$0")/smooth.awk -v who=qp2 -v label="smooth: VM player as the host draws them, at the frame before" "$h"
		awk -f $(dirname "$0")/smooth.awk -v who=fme -v label="smooth: host player in their own game, frame time" "$h"
	fi
	svg "$v" me "$h" p2 $O/vm-as-host-sees.svg; svg "$h" me "$v" p1 $O/host-as-vm-sees.svg
	echo "plots: $O/vm-as-host-sees.svg $O/host-as-vm-sees.svg"
}

clock() {
	clock_resync
	local ms; ms=$(clock_offset) || { echo "VM clock: could not measure it (ssh)"; return 1; }
	if [ $(abs $ms) -gt $MAX_CLOCK_MS ]; then
		echo "VM clock: $ms ms from the host's (VM minus host): over $MAX_CLOCK_MS ms, run and analyse will refuse"; return 1
	fi
	echo "VM clock: $ms ms from the host's (VM minus host)"
}

case "$1" in
run) run $2 ;;
analyse) analyse $2 $3 $4 ;;
npcs) npcs $2 $3 ;;
clock) clock ;;
*) echo "usage: trace.sh run [seconds] | analyse [host.csv vm.csv [vmClockMs]] | npcs <host.csv> <vm.csv> | clock" ;;
esac
