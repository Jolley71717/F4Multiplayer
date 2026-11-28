# moveaudit.awk: one movement segment measured (moveaudit.sh calls it once a segment and explains the thresholds).
#   awk -f moveaudit.awk -v kind=run -v t0=<ms> -v t1=<ms> -v ownOff=<ms> -v viewOff=<ms> -v ownF=own.csv -v viewF=view.csv \
#       -v animF=anim.tsv -v markF=marks.tsv  own.csv view.csv FS='\t' anim.tsv marks.tsv
# own.csv:  the mover's game's trace (dev `trace`: ms,who,x,y,z,rz,speed); its "me" rows are the mover's own path.
# view.csv: the watcher's game's trace; its p<id> rows are the stand-in as that game last put it.
# anim.tsv: what the watcher's game said of the stand-in while it moved: "<ms> A <dev anim reply>" and "<ms> E <animation events>".
# marks.tsv: this segment's marks: "<ms> <mover> <group> <segment> <event> <text>" (probe: the mover's own `anim`; mev: the mover's own
#           animation events over the segment).
# snapSkip=<ms>: snaps in the first ms of the segment are not counted (a segment that opens with a console placement, said in the run).
# noGraph=1: the graph of the mover itself is not compared (first person: the graph read there is not the body a friend sees).
# Every time is brought to the host's clock: ownOff and viewOff are each trace's machine's clock minus the host's.
# Prints one line: "<PASS|FAIL|VOID>\t<why>\t<name=value ...>".
function abs(v) { return v < 0 ? -v : v }
function wrap(d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d }
function val(s, n,   i, r) { i = index(s, " " n "="); if (!i) return ""; r = substr(s, i + length(n) + 2); sub(/[ ;].*$/, "", r); return r }
function num(s) { sub(/[ib]$/, "", s); return s }
# the owner's sample at time t (interpolated): OX OY OZ OH
function oidx(t,   lo, hi, mid) {
	if (t <= ot[1]) return 1; if (t >= ot[on]) return on - 1
	lo = 1; hi = on
	while (hi - lo > 1) { mid = int((lo + hi) / 2); if (ot[mid] <= t) lo = mid; else hi = mid }
	return lo
}
function oat(t,   j, f) {
	j = oidx(t); f = (ot[j + 1] == ot[j]) ? 0 : (t - ot[j]) / (ot[j + 1] - ot[j]); if (f < 0) f = 0; if (f > 1) f = 1
	OX = ox[j] + (ox[j + 1] - ox[j]) * f; OY = oy[j] + (oy[j + 1] - oy[j]) * f; OZ = oz[j] + (oz[j + 1] - oz[j]) * f
	OH = oh[j] + wrap(oh[j + 1] - oh[j]) * f; OV = osp[j]
	return j
}
# speeds from positions over about 120 ms around each sample (u/s)
function speeds(n, t, x, y, out,   i, j, k, dt) {
	for (i = 1; i <= n; i++) {
		j = i; while (j > 1 && t[i] - t[j] < 60) j--
		k = i; while (k < n && t[k] - t[i] < 60) k++
		dt = t[k] - t[j]; out[i] = dt > 0 ? sqrt((x[k] - x[j]) ^ 2 + (y[k] - y[j]) ^ 2) / dt * 1000 : 0
	}
}
# spells of movement: starts over START u/s, ends under STOP u/s (the mod's own start and stop speeds); gaps under 150 ms are
# one spell, spells under 200 ms are none. Fills S[1..] and E[1..], returns the count.
function spells(n, t, v, S, E,   i, c, m, s, k, c2) {
	c = 0; m = 0
	for (i = 1; i <= n; i++) {
		if (!m && v[i] > START) { m = 1; s = t[i] }
		else if (m && v[i] < STOP) { m = 0; c++; S[c] = s; E[c] = t[i] }
	}
	if (m) { c++; S[c] = s; E[c] = t[n] }
	c2 = 0
	for (k = 1; k <= c; k++) {
		if (c2 > 0 && S[k] - E[c2] < 150) { E[c2] = E[k]; continue }
		c2++; S[c2] = S[k]; E[c2] = E[k]
	}
	c = 0
	for (k = 1; k <= c2; k++) if (E[k] - S[k] >= 200) { c++; S[c] = S[k]; E[c] = E[k] }
	return c
}
function in_spell(t, S, E, c, pad,   k) { for (k = 1; k <= c; k++) if (t >= S[k] + pad && t <= E[k] - pad) return k; return 0 }
# the 90th percentile of v[1..n] (whole units)
function p90(n, v, q,   i, h, c, b) {
	delete h; for (i = 1; i <= n; i++) h[int(v[i])]++
	c = 0; for (b = 0; b <= 2000; b++) { c += h[b]; if (c >= n * q) return b }
	return 2000
}
function mode_add(arr, k) { if (k != "") arr[k]++ }
function mode_of(arr,   k, b, bn) { b = ""; bn = 0; for (k in arr) if (arr[k] > bn) { bn = arr[k]; b = k } return b }
function feet(s,   n, a, i, c, tag) { c = 0; n = split(s, a, " "); for (i = 1; i <= n; i++) { tag = a[i]; sub(/\(.*/, "", tag); if (tag ~ /^[Ff]oot.*(Left|Right)/) c++ } return c }
function tags(s, into,   n, a, i, tag) { n = split(s, a, " "); for (i = 1; i <= n; i++) { tag = a[i]; sub(/\(.*/, "", tag); if (tag != "" && tag != "none") into[tag]++ } }
function tag_list(arr,   k, out, c, best, bn, used) {
	out = ""
	for (c = 0; c < 8; c++) {
		best = ""; bn = 0
		for (k in arr) if (!(k in used) && arr[k] > bn) { bn = arr[k]; best = k }
		if (best == "") break
		used[best] = 1; out = out (out == "" ? "" : ",") best "x" bn
	}
	return out == "" ? "none" : out
}
function fail(why) { fails = fails (fails == "" ? "" : "; ") why }
function note(why) { notes = notes (notes == "" ? "" : "; ") why }
function out(n, v) { metrics = metrics " " n "=" v }

BEGIN {
	FS = ","
	if (START == "") START = 25; if (STOP == "") STOP = 10
	if (T_LAG == "") T_LAG = 300; if (T_ERR_MEAN == "") T_ERR_MEAN = 12; if (T_ERR_P95 == "") T_ERR_P95 = 35
	if (T_CROSS == "") T_CROSS = 10; if (T_ZERR == "") T_ZERR = 25; if (T_PATH_LO == "") T_PATH_LO = 0.93; if (T_PATH_HI == "") T_PATH_HI = 1.08
	if (T_BACK == "") T_BACK = 6; if (T_REACT == "") T_REACT = 400; if (T_OVER == "") T_OVER = 25; if (T_SNAPBACK == "") T_SNAPBACK = 10
	if (T_REST == "") T_REST = 10; if (T_HEAD == "") T_HEAD = 15; if (T_HEAD_END == "") T_HEAD_END = 10; if (T_MOVFRAC == "") T_MOVFRAC = 0.85
	if (T_SWING == "") T_SWING = 0.05; if (T_CAD_LO == "") T_CAD_LO = 0.7; if (T_CAD_HI == "") T_CAD_HI = 1.4; if (T_REST_MOV == "") T_REST_MOV = 0.15
	if (T_SIT == "") T_SIT = 2500; if (T_SNEAK == "") T_SNEAK = 700; if (T_SNAP == "") T_SNAP = 25; if (LAG_MAX == "") LAG_MAX = 1000; snapSkip += 0
	lo = t0 - 600; hi = t1 + 600
}
FILENAME == ownF {
	if ($2 != "me") next
	t = $1 - ownOff; if (t < lo || t > hi) next
	on++; ot[on] = t; ox[on] = $3; oy[on] = $4; oz[on] = $5; oh[on] = $6; osp[on] = $7; next
}
FILENAME == viewF {
	if ($2 !~ /^p[0-9]+$/) next
	t = $1 - viewOff; if (t < lo || t > hi) next
	vn++; vt[vn] = t; vx[vn] = $3; vy[vn] = $4; vz[vn] = $5; vh[vn] = $6; next
}
FILENAME == animF {
	if ($1 < t0 || $1 > hi) next
	if ($2 == "A") { an++; at[an] = $1; al[an] = $3 }
	else if ($2 == "E") { en++; et[en] = $1; el[en] = $3 }
	next
}
FILENAME == markF {
	if ($5 == "probe") { pn++; pt[pn] = $1; pl[pn] = $6 }
	else if ($5 == "mev") mev = mev " " $6
	else if ($5 == "key") { if ($6 ~ /front=no/) dropped++; if ($6 ~ /^0x11 /) { kc++; kct[kc] = $1 } }
	else if ($5 == "msit") { sn++; stt[sn] = $1; sst[sn] = $6 }
	next
}
END {
	fails = ""; notes = ""; metrics = ""
	if (on < 20 || vn < 20) { printf "VOID\tnot enough samples (the mover's own %d, the stand-in's %d)\tn_o=%d n_v=%d\n", on, vn, on, vn; exit }
	speeds(on, ot, ox, oy, ov); speeds(vn, vt, vx, vy, vv)
	# what the mover did
	pathO = 0; for (i = 2; i <= on; i++) if (ot[i] >= t0 && ot[i] <= t1) pathO += sqrt((ox[i] - ox[i - 1]) ^ 2 + (oy[i] - oy[i - 1]) ^ 2)
	pathV = 0; for (i = 2; i <= vn; i++) if (vt[i] >= t0 && vt[i] <= hi) pathV += sqrt((vx[i] - vx[i - 1]) ^ 2 + (vy[i] - vy[i - 1]) ^ 2)
	m = 0; for (i = 1; i <= on; i++) if (ov[i] > START) { m++; tmp[m] = ov[i] }
	v90o = m ? p90(m, tmp, 0.9) : 0; v50o = m ? p90(m, tmp, 0.5) : 0; delete tmp
	m = 0; for (i = 1; i <= vn; i++) if (vv[i] > START) { m++; tmp[m] = vv[i] }
	v90v = m ? p90(m, tmp, 0.9) : 0; v50v = m ? p90(m, tmp, 0.5) : 0; delete tmp
	peak = 100; for (i = 1; i <= on; i++) if (ov[i] > peak) peak = ov[i]
	so = spells(on, ot, ov, oS, oE); sv = spells(vn, vt, vv, vS, vE)
	# heights: each one's own level at the segment's start
	bo = 0; c = 0; for (i = 1; i <= on; i++) if (ot[i] >= t0 && ot[i] <= t0 + 150) { bo += oz[i]; c++ } if (c) bo /= c; else bo = oz[1]
	bv = 0; c = 0; for (i = 1; i <= vn; i++) if (vt[i] >= t0 && vt[i] <= t0 + 150) { bv += vz[i]; c++ } if (c) bv /= c; else bv = vz[1]
	riseO = 0; dropO = 0; for (i = 1; i <= on; i++) if (ot[i] >= t0 && ot[i] <= t1) { if (oz[i] - bo > riseO) riseO = oz[i] - bo; if (bo - oz[i] > dropO) dropO = bo - oz[i] }
	riseV = 0; dropV = 0; for (i = 1; i <= vn; i++) if (vt[i] >= t0 && vt[i] <= hi) { if (vz[i] - bv > riseV) riseV = vz[i] - bv; if (bv - vz[i] > dropV) dropV = bv - vz[i] }
	if (kind == "fall") {   # a fall is read in the world's own heights: from the highest point in the window to where each came to rest
		bo = 0; bv = 0; riseO = 0; riseV = 0; io = 0; iv = 0
		for (i = 1; i <= on; i++) if (ot[i] >= t0 && ot[i] <= t1 && (!io || oz[i] > oz[io])) io = i
		for (i = 1; i <= vn; i++) if (vt[i] >= t0 && vt[i] <= hi && (!iv || vz[i] > vz[iv])) iv = i
		dropO = io ? oz[io] - oz[on] : 0; dropV = iv ? vz[iv] - vz[vn] : 0; tlo = 0; tlv = 0
		for (i = io; io && i <= on; i++) if (oz[i] - oz[on] < 5) { tlo = ot[i]; break }
		for (i = iv; iv && i <= vn; i++) if (vz[i] - vz[vn] < 5) { tlv = vt[i]; break }
		if (tlo && tlv) landMs = tlv - tlo
	}
	hmin = 0; hmax = 0; hc = 0; for (i = 2; i <= on; i++) if (ot[i] >= t0 && ot[i] <= t1) { hc += wrap(oh[i] - oh[i - 1]); if (hc < hmin) hmin = hc; if (hc > hmax) hmax = hc }
	turned = hmax - hmin
	out("n_o", on); out("n_v", vn); out("path_o", sprintf("%.0f", pathO)); out("path_v", sprintf("%.0f", pathV))
	out("v50_o", v50o); out("v90_o", v90o); out("v50_v", v50v); out("v90_v", v90v); out("spells_o", so); out("spells_v", sv)
	out("rise_o", sprintf("%.0f", riseO)); out("rise_v", sprintf("%.0f", riseV)); out("drop_o", sprintf("%.0f", dropO)); out("drop_v", sprintf("%.0f", dropV))
	out("turned_o", sprintf("%.0f", turned))
	# the mover's own graph at the probe (the first one)
	if (pn > 0) { mmO = val(pl[1], "moveMode"); gaitO = num(val(pl[1], "iSyncLocomotionSpeed")); sneakO = num(val(pl[1], "iIsInSneak")); out("mm_o", mmO); out("gait_o", gaitO); out("sneak_o", sneakO) }
	if (dropped) note(dropped " key press(es) went out with the game not in front")

	# VOID: the mover did not do what the segment is for (the rig's, or the game's own input: never a verdict on the stand-in)
	# sitting: when the mover was first seated (sit state 3) and first back on its feet (0) after that, by its own game's readings
	sitO = ""; standO = ""; for (i = 1; i <= sn; i++) { if (sitO == "" && sst[i] == 3) sitO = stt[i]; else if (sitO != "" && standO == "" && sst[i] == 0) standO = stt[i] }
	void = ""
	moves = (kind != "sneakstill" && kind != "turn" && kind != "jumpstand" && kind != "sit" && kind != "still")
	if (moves && pathO < 100) void = sprintf("the mover did not move (%.0f u)", pathO)
	else if (kind == "walk" && (v50o < 50 || v50o > 170)) void = sprintf("the mover was not walking (its usual speed %d u/s)", v50o)
	else if ((kind == "run" || kind == "stopstart" || kind == "runturn" || kind == "jumprun") && (v50o < 230 || v50o > 440)) void = sprintf("the mover was not running (its usual speed %d u/s)", v50o)
	else if (kind == "sprint" && v90o < 440) void = sprintf("the mover did not sprint (its fastest %d u/s)", v90o)
	else if ((kind == "jumpstand" || kind == "jumprun") && riseO < 20) void = sprintf("the mover did not jump (rose %.0f u)", riseO)
	else if (kind == "fall" && dropO < 120) void = sprintf("the mover did not fall (dropped %.0f u)", dropO)
	else if (kind == "turn" && turned < 60) void = sprintf("the mover did not turn (%.0f degrees)", turned)
	else if (kind == "sit" && sitO == "") void = "the mover never sat (" sn " readings of its sit state, none 3)"
	else if (kind == "adswalk" && mev !~ /sightedStateEnter/) { delete to; tags(mev, to); void = "the mover did not aim (no sightedStateEnter among its own events: " tag_list(to) ")" }
	else if ((kind == "sneakrun" || kind == "sneakwalk" || kind == "sneakstill") && pn > 0 && sneakO != "" && sneakO != "1") void = "the mover was not sneaking at the probe (iIsInSneak=" sneakO ")"
	if (void != "") { printf "VOID\t%s\t%s\n", void, substr(metrics, 2); exit }

	# the lag: the time shift that lines the stand-in's path up with the mover's (by place when the mover moved, by heading when it
	# only turned)
	byPlace = (pathO >= 50); byHead = (!byPlace && turned >= 30)
	lag = ""
	if (byPlace || byHead) {
		best = -1
		for (pass = 1; pass <= 2; pass++) {
			if (pass == 1) { a = 0; b = LAG_MAX; st = 20 } else { a = lag - 20; if (a < 0) a = 0; b = lag + 20; st = 5 }
			for (sh = a; sh <= b; sh += st) {
				s = 0; c = 0
				for (i = 1; i <= vn; i++) {
					t = vt[i] - sh; if (vt[i] < t0 || vt[i] > hi || t < ot[1] || t > ot[on]) continue
					oat(t); s += byPlace ? sqrt((OX - vx[i]) ^ 2 + (OY - vy[i]) ^ 2) : abs(wrap(vh[i] - OH)); c++
				}
				if (c < 10) continue
				if (best < 0 || s / c < best) { best = s / c; lag = sh }
			}
		}
	}
	L = (lag == "") ? 120 : lag   # nothing to fit: the usual delay, for the readings below
	# errors at that lag
	es = 0; ec = 0; emax = 0; zs = 0; zmax = 0; hs = 0; hmx = 0; cs = 0; cc = 0; cmax = 0; delete eh
	hends = 0; hendc = 0
	for (i = 1; i <= vn; i++) {
		t = vt[i] - L; if (vt[i] < t0 || vt[i] > hi || t < ot[1] || t > ot[on]) continue
		j = oat(t); d = sqrt((OX - vx[i]) ^ 2 + (OY - vy[i]) ^ 2); es += d; ec++; if (d > emax) emax = d; eh[int(d)]++
		dz = abs(vz[i] - bv - (OZ - bo)); zs += dz; if (dz > zmax) zmax = dz
		dh = abs(wrap(vh[i] - OH)); hs += dh; if (dh > hmx) hmx = dh
		if (vt[i] >= t1 - 300) { hends += dh; hendc++ }
		# across the mover's line of travel (cutting a corner, drifting beside the path), while the mover moves
		if (ov[j] > 100 && j > 1 && j < on) {
			k1 = j; while (k1 > 1 && ot[j] - ot[k1] < 60) k1--; k2 = j; while (k2 < on && ot[k2] - ot[j] < 60) k2++
			ddx = ox[k2] - ox[k1]; ddy = oy[k2] - oy[k1]; dl = sqrt(ddx * ddx + ddy * ddy)
			if (dl > 1) { cr = abs((vx[i] - OX) * ddy - (vy[i] - OY) * ddx) / dl; cs += cr; cc++; if (cr > cmax) cmax = cr }
		}
	}
	if (ec == 0) { printf "VOID\tthe two traces do not overlap in time (clocks?)\t%s\n", substr(metrics, 2); exit }
	errMean = es / ec; c = 0; errP95 = 0; for (b = 0; b <= 5000; b++) { c += eh[b]; if (c >= ec * 0.95) { errP95 = b; break } }
	zMean = zs / ec; headMean = hs / ec; headEnd = hendc ? hends / hendc : 0; cross = cc ? cs / cc : 0
	out("lag", lag == "" ? "none" : lag); out("errMean", sprintf("%.1f", errMean)); out("errP95", errP95); out("errMax", sprintf("%.0f", emax))
	out("cross", sprintf("%.1f", cross)); out("crossMax", sprintf("%.0f", cmax)); out("zErr", sprintf("%.1f", zMean)); out("zErrMax", sprintf("%.0f", zmax))
	out("headMean", sprintf("%.1f", headMean)); out("headMax", sprintf("%.0f", hmx)); out("headEnd", sprintf("%.1f", headEnd))

	# snaps (one frame's step far beyond what the mover's top speed covers in that frame) and steps backwards along the mover's
	# line of travel (while the mover moves, or within 1.5 s of its stop)
	snaps = 0; snapMax = 0; backN = 0; backMax = 0; ldx = 0; ldy = 0; lastMove = -1e15
	for (i = 2; i <= vn; i++) {
		if (vt[i] < t0 || vt[i] > hi) continue
		dt = vt[i] - vt[i - 1]; if (dt <= 0) continue
		dx = vx[i] - vx[i - 1]; dy = vy[i] - vy[i - 1]; st = sqrt(dx * dx + dy * dy)
		lim = 3 * peak * dt / 1000; if (lim < T_SNAP) lim = T_SNAP
		if (st > lim && vt[i] >= t0 + snapSkip) { snaps++; if (st > snapMax) snapMax = st }
		t = vt[i] - L; if (t < ot[1] || t > ot[on]) continue
		j = oidx(t)
		if (ov[j] > 60) {
			k1 = j; while (k1 > 1 && ot[j] - ot[k1] < 60) k1--; k2 = j; while (k2 < on && ot[k2] - ot[j] < 60) k2++
			ddx = ox[k2] - ox[k1]; ddy = oy[k2] - oy[k1]; dl = sqrt(ddx * ddx + ddy * ddy)
			if (dl > 1) { ldx = ddx / dl; ldy = ddy / dl; lastMove = t }
		}
		if (t - lastMove <= 1500) { bk = -(dx * ldx + dy * ldy); if (bk > 4) { backN++; if (bk > backMax) backMax = bk } }
	}
	out("snaps", snaps); out("snapMax", sprintf("%.0f", snapMax)); out("backSteps", backN); out("backMax", sprintf("%.0f", backMax))
	out("pathRatio", pathO > 50 ? sprintf("%.2f", pathV / pathO) : "none")

	# starts and stops: how long after the mover the stand-in sets off and comes to rest; how far past the mover's resting place it
	# goes and how far it comes back
	rsN = 0; rsSum = 0; rsMax = -1e9; rsMin = 1e9; rpSum = 0; rpMax = -1e9; over = 0; snapback = 0
	if (so > 0 && so == sv) {
		for (k = 1; k <= so; k++) {
			d = vS[k] - oS[k]; rsSum += d; if (d > rsMax) rsMax = d; if (d < rsMin) rsMin = d
			d = vE[k] - oE[k]; rpSum += d; if (d > rpMax) rpMax = d; rsN++
		}
	}
	for (k = 1; k <= so; k++) {
		if (oE[k] + 400 > ot[on]) continue
		oat(oE[k] + 400); px = OX; py = OY; oat(oE[k] - 300); ddx = px - OX; ddy = py - OY; dl = sqrt(ddx * ddx + ddy * ddy); if (dl < 5) continue
		ddx /= dl; ddy /= dl; until = (k < so) ? oS[k + 1] : hi; mx = -1e9; last = 0; c = 0
		for (i = 1; i <= vn; i++) if (vt[i] >= oE[k] && vt[i] <= until) { s = (vx[i] - px) * ddx + (vy[i] - py) * ddy; if (s > mx) mx = s; last = s; c++ }
		if (c == 0) continue
		if (mx > over) over = mx
		if (mx > 0 && mx - last > snapback) snapback = mx - last
	}
	oat(ot[on]); restErr = sqrt((OX - vx[vn]) ^ 2 + (OY - vy[vn]) ^ 2)
	if (rsN) { out("startMs", sprintf("%.0f", rsSum / rsN)); out("startMsMax", sprintf("%.0f", rsMax)); out("stopMs", sprintf("%.0f", rpSum / rsN)); out("stopMsMax", sprintf("%.0f", rpMax)) }
	out("overshoot", sprintf("%.0f", over)); out("snapback", sprintf("%.0f", snapback)); out("restErr", sprintf("%.0f", restErr))

	# the stand-in's animation, as the watcher's game told it: while the mover moves (0.3 s in from each end of a spell, at the lag)
	# and while it rests (from 0.8 s after a stop)
	mN = 0; mMov = 0; rN = 0; rMov = 0; delete gm; delete mmm; delete snk; delete lmin; delete lmax; delete rmin; delete rmax; gs = 0; gsn = 0
	for (i = 1; i <= an; i++) {
		t = at[i] - L; line = al[i]
		split(val(line, "leg"), lg, /[(),]/)   # "(a,b,c)": lg[2..4]
		if (in_spell(t, oS, oE, so, 300)) {
			mN++; if (val(line, "moving") == "true") mMov++
			mode_add(gm, num(val(line, "iSyncLocomotionSpeed"))); mode_add(mmm, val(line, "moveMode")); mode_add(snk, num(val(line, "iIsInSneak")))
			g = val(line, "Speed"); if (g != "" && g != "?") { gs += g; gsn++ }
			ch = int((t - t0) / 1000)   # the swing second by second: a glide for part of the way shows in its worst second
			for (q = 2; q <= 4; q++) if (lg[q] != "") {
				if (!((ch, q) in lmin) || lg[q] < lmin[ch, q]) lmin[ch, q] = lg[q]
				if (!((ch, q) in lmax) || lg[q] > lmax[ch, q]) lmax[ch, q] = lg[q]
				chs[ch]++
			}
		} else {
			rest = 1; for (k = 1; k <= so; k++) if (t >= oS[k] - 200 && t <= oE[k] + 800) rest = 0
			if (rest && at[i] <= t1 + L) {
				rN++; if (val(line, "moving") == "true") rMov++
				for (q = 2; q <= 4; q++) if (lg[q] != "") { if (!(q in rmin) || lg[q] < rmin[q]) rmin[q] = lg[q]; if (!(q in rmax) || lg[q] > rmax[q]) rmax[q] = lg[q] }
			}
		}
	}
	swing = 0; swingWorst = 1e9
	for (ch in chs) {
		if (chs[ch] < 9) continue   # a second with too few readings says nothing
		s = 0; for (q = 2; q <= 4; q++) if ((ch, q) in lmin && lmax[ch, q] - lmin[ch, q] > s) s = lmax[ch, q] - lmin[ch, q]
		if (s > swing) swing = s; if (s < swingWorst) swingWorst = s
	}
	if (swingWorst > 1e8) swingWorst = ""
	restSwing = 0; for (q = 2; q <= 4; q++) if (q in rmin && rmax[q] - rmin[q] > restSwing) restSwing = rmax[q] - rmin[q]
	gaitV = mode_of(gm); mmV = mode_of(mmm); sneakV = mode_of(snk)
	fv = 0; delete tv; for (i = 1; i <= en; i++) if (et[i] <= t1 + L + 300) { fv += feet(el[i]); tags(el[i], tv) }
	fo = feet(mev); delete to; tags(mev, to)
	out("animN", mN); if (mN) { out("movingFrac", sprintf("%.2f", mMov / mN)); out("swing", sprintf("%.3f", swing)); out("swingWorst", swingWorst == "" ? "none" : sprintf("%.3f", swingWorst)) }
	if (rN) { out("restN", rN); out("restMovingFrac", sprintf("%.2f", rMov / rN)); out("restSwing", sprintf("%.3f", restSwing)) }
	out("gait_v", gaitV == "" ? "none" : gaitV); out("mm_v", mmV == "" ? "none" : mmV); out("sneak_v", sneakV == "" ? "none" : sneakV)
	if (gsn) out("graphSpeed_v", sprintf("%.0f", gs / gsn))
	out("feet_o", fo); out("feet_v", fv)
	if (fo > 0 && fv > 0 && pathO > 50) { out("stride_o", sprintf("%.0f", pathO / fo)); out("stride_v", sprintf("%.0f", pathV / fv)) }
	out("events_o", tag_list(to)); out("events_v", tag_list(tv))
	# sneaking: what the stand-in's graph says at the mover's probe, and how long after each press of the sneak key it changed
	sneakP = ""; sneakOn = ""; sneakOff = ""
	if (pn > 0 && sneakO != "" && sneakO != "?") {
		bestd = 1e9
		for (i = 1; i <= an; i++) { d = abs(at[i] - (pt[1] + L)); if (d < bestd) { bestd = d; sneakP = num(val(al[i], "iIsInSneak")) } }
		if (bestd > 300) sneakP = ""; else out("sneakAtProbe_v", sneakP)
	}
	if (kc >= 1) { for (i = 1; i <= an; i++) if (at[i] >= kct[1] && num(val(al[i], "iIsInSneak")) == "1") { sneakOn = at[i] - kct[1]; break } out("sneakOnMs", sneakOn == "" ? "never" : sneakOn) }
	if (kc >= 2) { for (i = 1; i <= an; i++) if (at[i] >= kct[kc] && num(val(al[i], "iIsInSneak")) == "0") { sneakOff = at[i] - kct[kc]; break } out("sneakOffMs", sneakOff == "" ? "never" : sneakOff) }
	if (landMs != "") out("landMs", sprintf("%.0f", landMs))
	# sitting: the stand-in's own sit state (3 seated, 0 on its feet) against the mover's, each read about four times a second
	if (kind == "sit") {
		sitV = ""; standV = ""
		for (i = 1; i <= an; i++) { s = val(al[i], "sit"); if (sitV == "" && at[i] >= t0 && s == 3) sitV = at[i]; else if (sitV != "" && standV == "" && standO != "" && at[i] >= standO - 300 && s == 0) standV = at[i] }
		out("sitMs", sitV == "" ? "never" : sitV - sitO); if (standO != "") out("standMs", standV == "" ? "never" : standV - standO)
		if (sitV == "") fail("never sat down while the mover sat"); else if (sitV - sitO > T_SIT) fail(sprintf("sat down %d ms after the mover (limit %d)", sitV - sitO, T_SIT))
		if (standO != "" && sitV != "") { if (standV == "") fail("stayed seated after the mover got up"); else if (standV - standO > T_SIT) fail(sprintf("got up %d ms after the mover (limit %d)", standV - standO, T_SIT)) }
		if (standO == "") note("the mover did not get up in the window")
	}

	# the verdict
	if (sneakP != "" && sneakP != "?" && sneakP != sneakO) fail("at the mover's probe its sneak is " sneakP " where the mover's own is " sneakO)
	if (kind ~ /^sneak/ && kc >= 1 && an > 20) {
		if (sneakOn == "") fail("never went into a sneak")
		else if (sneakOn > T_SNEAK) fail(sprintf("went into its sneak %d ms after the key (limit %d)", sneakOn, T_SNEAK))
		if (kc >= 2 && sneakOff == "") fail("never came out of its sneak")
		else if (kc >= 2 && sneakOff > T_SNEAK) fail(sprintf("came out of its sneak %d ms after the key (limit %d)", sneakOff, T_SNEAK))
	}
	if (kind == "fall" && landMs != "" && (landMs > T_REACT || landMs < -50)) fail(sprintf("landed %.0f ms after the mover (limit %d)", landMs, T_REACT))
	if (lag != "" && lag > T_LAG) fail(sprintf("drawn %d ms behind the mover (limit %d)", lag, T_LAG))
	if (byPlace) {
		if (errMean > T_ERR_MEAN) fail(sprintf("%.0f u from the mover's path on average once the delay is taken out (limit %d)", errMean, T_ERR_MEAN))
		if (errP95 > T_ERR_P95) fail(sprintf("over %d u off for a twentieth of the time (limit %d)", errP95, T_ERR_P95))
		if (cross > T_CROSS) fail(sprintf("%.0f u beside the mover's line on average (limit %d)", cross, T_CROSS))
		if (pathO > 200 && (pathV / pathO < T_PATH_LO || pathV / pathO > T_PATH_HI)) fail(sprintf("covered %.2f times the mover's distance (limits %.2f to %.2f)", pathV / pathO, T_PATH_LO, T_PATH_HI))
	} else if (restErr > T_REST && kind != "sit") fail(sprintf("stands %.0f u from where the mover stands (limit %d)", restErr, T_REST))
	if (kind == "jumpstand" || kind == "jumprun" || kind == "fall") {
		big = (kind == "fall") ? dropO : riseO; got = (kind == "fall") ? dropV : riseV
		if (got < big * 0.6 || got > big * 1.4) fail(sprintf("%s %.0f u where the mover's own was %.0f u", kind == "fall" ? "dropped" : "rose", got, big))
		if (zMean > T_ZERR) fail(sprintf("%.0f u off in height on average (limit %d)", zMean, T_ZERR))
	} else if (zmax > 2 * T_ZERR) fail(sprintf("up to %.0f u off in height (limit %d)", zmax, 2 * T_ZERR))
	if (snaps > 0) fail(sprintf("%d snap(s), the largest %.0f u in one frame", snaps, snapMax))
	if (backMax > T_BACK) fail(sprintf("%d step(s) backwards, the largest %.0f u (limit %d)", backN, backMax, T_BACK))
	if (so != sv && moves) fail(sprintf("set off and stopped %d time(s) for the mover's %d", sv, so))
	if (rsN) {
		if (rsMax > T_REACT) fail(sprintf("set off %.0f ms after the mover (limit %d)", rsMax, T_REACT))
		if (rpMax > T_REACT) fail(sprintf("came to rest %.0f ms after the mover (limit %d)", rpMax, T_REACT))
		if (rsMin < -50) fail(sprintf("set off %.0f ms before the mover: the clocks, or a guess ahead", -rsMin))
	}
	if (over > T_OVER) fail(sprintf("ran %.0f u past where the mover stopped (limit %d)", over, T_OVER))
	if (snapback > T_SNAPBACK) fail(sprintf("came back %.0f u after overshooting (limit %d)", snapback, T_SNAPBACK))
	if (moves && restErr > T_REST && so > 0) fail(sprintf("came to rest %.0f u from the mover (limit %d)", restErr, T_REST))
	if (kind == "turn" || kind == "runturn") { if (headMean > T_HEAD) fail(sprintf("faces %.0f degrees off the mover on average (limit %d)", headMean, T_HEAD)) }
	if (headEnd > T_HEAD_END) fail(sprintf("ends facing %.0f degrees off the mover (limit %d)", headEnd, T_HEAD_END))
	if (mN >= 8) {
		if (mMov / mN < T_MOVFRAC) fail(sprintf("its legs were told to move for %.0f%% of the time the mover moved (limit %.0f%%)", 100 * mMov / mN, 100 * T_MOVFRAC))
		if (swingWorst != "" && swingWorst < T_SWING) fail(sprintf("glides: its legs barely swing for a second or more while it moves (swing %.3f, limit %.2f)", swingWorst, T_SWING))
		if (pn > 0 && !noGraph && gaitO != "" && gaitO != "?" && gaitV != "" && gaitV != "?" && gaitO != gaitV) fail("plays gait " gaitV " where the mover's own graph plays " gaitO " (0 walk, 1 jog, 2 run)")
		if (pn > 0 && !noGraph && sneakO != "" && sneakO != "?" && sneakV != "" && sneakV != "?" && sneakO != sneakV) fail("sneak " sneakV " where the mover's own is " sneakO)
		if (pn > 0 && mmO != "" && mmV != "" && mmO != mmV) note("move mode " mmV " for the mover's " mmO)
	} else if (moves && an > 0) note("too few readings of the stand-in's graph while it moved (" mN ")")
	if (fo >= 4 && kind != "sit") {   # a few steps to a chair are too few to compare
		r = fv / fo
		if (r < T_CAD_LO || r > T_CAD_HI) fail(sprintf("%d footfalls for the mover's own %d (limits %.1f to %.1f times)", fv, fo, T_CAD_LO, T_CAD_HI))
	} else if (moves && fv == 0 && en > 0 && pathO > 300) fail("no footfall at all while it covered " sprintf("%.0f", pathV) " u")
	if (rN >= 8) {
		if (rMov / rN > T_REST_MOV) fail(sprintf("its legs were still told to move %.0f%% of the time the mover stood", 100 * rMov / rN))
		if (restSwing > 4 * T_SWING && moves) note(sprintf("legs swing %.3f while the mover stands", restSwing))
	}
	printf "%s\t%s\t%s\n", fails == "" ? "PASS" : "FAIL", (fails == "" ? "ok" : fails) (notes == "" ? "" : " [" notes "]"), substr(metrics, 2)
}
