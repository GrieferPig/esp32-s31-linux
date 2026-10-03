#!/bin/sh
# Measure ESP32-S31 radio playback load against the actual elapsed CPU ticks.

set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
	echo "usage: $0 <s31-radio-pid> <btdm-pid> <audio-pid> [seconds]" >&2
	exit 2
fi

radio_pid=$1
btdm_pid=$2
audio_pid=$3
seconds=${4:-15}
capture_top=${S31_CPU_TOP:-0}
cpu_count=$(awk '/^cpu[0-9]+ / { n++ } END { print n + 0 }' /proc/stat)

task_ticks()
{
	[ -r "/proc/$1/stat" ] || { echo 0; return; }
	set -- $(cat "/proc/$1/stat")
	echo $((${14} + ${15}))
}

context_switches()
{
	[ -r "/proc/$1/status" ] || { echo "0 0"; return; }
	awk '
		$1 == "voluntary_ctxt_switches:" { voluntary = $2 }
		$1 == "nonvoluntary_ctxt_switches:" { nonvoluntary = $2 }
		END { print voluntary + 0, nonvoluntary + 0 }
	' "/proc/$1/status"
}

irq_count()
{
	awk -v irq="$1:" '
		$1 == irq {
			for (i = 2; i <= NF && $i ~ /^[0-9]+$/; i++) total += $i
			print total + 0
			found = 1
			exit
		}
		END { if (!found) print 0 }
	' /proc/interrupts
}

mem_available()
{
	awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo
}

snapshot_tasks()
{
	: > "$1"
	for stat in /proc/[0-9]*/stat; do
		# A short-lived task can disappear between glob expansion and open.
		# Keep the whole sample valid instead of aborting under set -e.
		[ -r "$stat" ] || continue
		awk '{ print $1, $14 + $15, $2 }' "$stat" >> "$1" 2>/dev/null || :
	done
}

snapshot_before="/tmp/s31-cpu.$$.before"
snapshot_after="/tmp/s31-cpu.$$.after"
trap 'rm -f "$snapshot_before" "$snapshot_after"' EXIT

# Collect the relatively expensive auxiliary data before opening the timed
# interval.  On this small target, six awk processes can otherwise add more
# than a second of sampler load to the result.
mem0=$(mem_available)
set -- $(context_switches "$radio_pid"); radio_v0=$1; radio_nv0=$2
set -- $(context_switches "$btdm_pid"); btdm_v0=$1; btdm_nv0=$2
set -- $(context_switches "$audio_pid"); audio_v0=$1; audio_nv0=$2
irq14_0=$(irq_count 14)
irq15_0=$(irq_count 15)
irq22_0=$(irq_count 22)
snapshot_tasks "$snapshot_before"
radio0=$(task_ticks "$radio_pid")
btdm0=$(task_ticks "$btdm_pid")
audio0=$(task_ticks "$audio_pid")
read -r _ u0 n0 s0 i0 w0 h0 q0 x0 _ < /proc/stat
read -r up0 _ < /proc/uptime

sleep "$seconds"

read -r up1 _ < /proc/uptime
read -r _ u1 n1 s1 i1 w1 h1 q1 x1 _ < /proc/stat
snapshot_tasks "$snapshot_after"
radio1=$(task_ticks "$radio_pid")
btdm1=$(task_ticks "$btdm_pid")
audio1=$(task_ticks "$audio_pid")
mem1=$(mem_available)
set -- $(context_switches "$radio_pid"); radio_v1=$1; radio_nv1=$2
set -- $(context_switches "$btdm_pid"); btdm_v1=$1; btdm_nv1=$2
set -- $(context_switches "$audio_pid"); audio_v1=$1; audio_nv1=$2
irq14_1=$(irq_count 14)
irq15_1=$(irq_count 15)
irq22_1=$(irq_count 22)
du=$((u1 - u0))
dn=$((n1 - n0))
ds=$((s1 - s0))
di=$((i1 - i0))
dw=$((w1 - w0))
dh=$((h1 - h0))
dq=$((q1 - q0))
dx=$((x1 - x0))
dt=$((du + dn + ds + di + dw + dh + dq + dx))
busy=$((du + dn + ds + dh + dq + dx))
radio_ticks=$((radio1 - radio0))
btdm_ticks=$((btdm1 - btdm0))
audio_ticks=$((audio1 - audio0))
other_ticks=$((busy - radio_ticks - btdm_ticks - audio_ticks))
task_sum_ticks=$(awk 'NR == FNR { old[$1] = $2; next }
	($1 in old) && $2 > old[$1] { total += $2 - old[$1] }
	END { print total + 0 }' "$snapshot_before" "$snapshot_after")

awk -v cpus="$cpu_count" -v up0="$up0" -v up1="$up1" \
	-v dt="$dt" -v busy="$busy" -v user="$((du + dn))" \
	-v sys_ticks="$ds" -v irq="$dh" -v softirq="$dq" \
	-v iowait="$dw" -v radio="$radio_ticks" -v btdm="$btdm_ticks" \
	-v audio="$audio_ticks" -v other="$other_ticks" \
	-v task_sum="$task_sum_ticks" '
	BEGIN {
		global_cores = cpus * busy / dt
		task_cores = cpus * task_sum / dt
		acceptance = global_cores > task_cores ? global_cores : task_cores
		printf "CPU elapsed=%.2fs ticks=%d cores=%.3f user=%.3f system=%.3f irq=%.3f softirq=%.3f iowait=%.3f\n", up1 - up0, dt, cpus * busy / dt, cpus * user / dt, cpus * sys_ticks / dt, cpus * irq / dt, cpus * softirq / dt, cpus * iowait / dt
		printf "TASK radio=%.3f btdm=%.3f audio=%.3f unassigned=%.3f\n", cpus * radio / dt, cpus * btdm / dt, cpus * audio / dt, cpus * other / dt
		printf "SCHED all_tasks=%.3f acceptance=%.3f\n", task_cores, acceptance
	}'
echo "CTX radio_v=$((radio_v1 - radio_v0)) radio_nv=$((radio_nv1 - radio_nv0)) btdm_v=$((btdm_v1 - btdm_v0)) btdm_nv=$((btdm_nv1 - btdm_nv0))"
echo "CTX audio_v=$((audio_v1 - audio_v0)) audio_nv=$((audio_nv1 - audio_nv0))"
echo "IRQ irq14=$((irq14_1 - irq14_0)) irq15=$((irq15_1 - irq15_0)) irq22=$((irq22_1 - irq22_0))"
echo "MEM available=${mem0}->${mem1}kB"
if [ "$capture_top" = 1 ]; then
	echo "TOP task_ticks pid comm"
	awk 'NR == FNR { old[$1] = $2; next } ($1 in old) && $2 > old[$1] { print $2 - old[$1], $1, $3 }' "$snapshot_before" "$snapshot_after" | sort -nr | head -10
fi
grep -E '^(VmSize|VmRSS):' "/proc/$audio_pid/status" 2>/dev/null || true
cat /proc/swaps
