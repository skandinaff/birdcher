#!/bin/bash
# Sample system state during a capture/stream soak.
#
#   soak-monitor.sh <outdir> <interval_secs>
#
# Emits one TSV row per interval so the run can be summarised afterwards
# without re-reading a wall of text. Run it alongside the stream; it stops
# when the marker file <outdir>/running disappears.
set -u
OUT="${1:?usage: soak-monitor.sh <outdir> <interval>}"
IV="${2:-30}"
mkdir -p "$OUT"

IFACE=$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'dev \K\S+' | head -1)
[ -z "$IFACE" ] && IFACE=wlan0

cpu_busy() {   # returns "busy total" jiffies from /proc/stat
    awk '/^cpu /{idle=$5+$6; tot=0; for(i=2;i<=NF;i++) tot+=$i; print tot-idle, tot; exit}' /proc/stat
}
tx_bytes() { awk -v i="$IFACE:" '$1==i{print $10; exit}' /proc/net/dev; }
temps()    { for z in /sys/class/thermal/thermal_zone*/temp; do printf "%s " "$(cat "$z" 2>/dev/null)"; done; }

read -r pb pt < <(cpu_busy)
ptx=$(tx_bytes)
t_prev=$(date +%s)

printf 'elapsed\tcpu_pct\tmem_avail_kB\tslab_kB\tsunreclaim_kB\tkstack_kB\ttx_kbps\ttemp_mC\n' > "$OUT/samples.tsv"
START=$t_prev

while [ -f "$OUT/running" ]; do
    sleep "$IV"
    [ -f "$OUT/running" ] || break
    read -r cb ct < <(cpu_busy)
    ctx=$(tx_bytes)
    t_now=$(date +%s)
    dt=$(( t_now - t_prev )); [ "$dt" -le 0 ] && dt=1

    cpu=$(awk -v a="$cb" -v b="$pb" -v c="$ct" -v d="$pt" 'BEGIN{ t=c-d; printf "%.1f", (t>0)? (a-b)*100.0/t : 0 }')
    kbps=$(awk -v a="$ctx" -v b="$ptx" -v s="$dt" 'BEGIN{ printf "%.0f", (a-b)*8/1000.0/s }')

    mem=$(awk '/^MemAvailable:/{ma=$2} /^Slab:/{sl=$2} /^SUnreclaim:/{su=$2} /^KernelStack:/{ks=$2} END{print ma"\t"sl"\t"su"\t"ks}' /proc/meminfo)

    printf '%s\t%s\t%s\t%s\t%s\n' "$(( t_now - START ))" "$cpu" "$mem" "$kbps" "$(temps)" >> "$OUT/samples.tsv"

    pb=$cb; pt=$ct; ptx=$ctx; t_prev=$t_now
done
