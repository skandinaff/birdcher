#!/bin/bash
# Enumerate every camera control as JSON, for a UI to build knobs from.
#
#   vim3-camera-controls.sh            # JSON to stdout
#   vim3-camera-controls.sh --set K=V  # set one control, then print JSON
#
# Covers both control surfaces, because they are genuinely different hardware:
#   /dev/video1        the ARM ISP  (brightness/AWB/AE/focus + isp_* extensions)
#   /dev/v4l-subdev0   the imx415   (blanking, analogue gain, test pattern)
#
# Frame rate note, since it is the one people reach for first:
# sensor vertical_blanking is the real rate control -- it changes the frame
# period, so the sensor and ISP genuinely do less work. isp_ds1_fps decimates
# in the DMA writer instead, dropping frames the ISP has already processed, so
# it costs the same power and heat. Prefer vertical_blanking.
#
#   total_lines = LINE_RATE / fps ; vertical_blanking = total_lines - 2192
#   LINE_RATE is 135000 lines/s at the current 3864x2192 preset.
set -u
VID=/dev/video1
SUB=/dev/v4l-subdev0

if [ "${1:-}" = "--set" ] && [ -n "${2:-}" ]; then
    k="${2%%=*}"; v="${2#*=}"
    if v4l2-ctl -d "$SUB" --list-ctrls 2>/dev/null | grep -q " $k "; then
        v4l2-ctl -d "$SUB" --set-ctrl="$k=$v" || exit 1
    else
        v4l2-ctl -d "$VID" --set-ctrl="$k=$v" || exit 1
    fi
    shift 2
fi

# v4l2-ctl prints e.g.
#   brightness 0x00980900 (int) : min=0 max=255 step=1 default=128 value=128 flags=slider
emit() {
    local dev="$1" surface="$2"
    v4l2-ctl -d "$dev" --list-ctrls 2>/dev/null | awk -v surface="$surface" '
        /^[[:space:]]*[a-z_0-9]+ 0x[0-9a-f]+ / {
            name=$1; id=$2; type=$3; gsub(/[()]/,"",type)
            min=""; max=""; step=""; def=""; val=""; flags=""
            for (i=4;i<=NF;i++) {
                split($i,kv,"=")
                if (kv[1]=="min") min=kv[2]
                else if (kv[1]=="max") max=kv[2]
                else if (kv[1]=="step") step=kv[2]
                else if (kv[1]=="default") def=kv[2]
                else if (kv[1]=="value") val=kv[2]
                else if (kv[1]=="flags") { flags=kv[2]; gsub(/,$/,"",flags) }
            }
            printf "    {\"name\":\"%s\",\"id\":\"%s\",\"type\":\"%s\",\"surface\":\"%s\"",
                   name, id, type, surface
            if (min!="")  printf ",\"min\":%s", min
            if (max!="")  printf ",\"max\":%s", max
            if (step!="") printf ",\"step\":%s", step
            if (def!="")  printf ",\"default\":%s", def
            if (val!="")  printf ",\"value\":%s", val
            if (flags!="") printf ",\"flags\":\"%s\"", flags
            printf "},\n"
        }'
}

vb=$(v4l2-ctl -d "$SUB" --get-ctrl=vertical_blanking 2>/dev/null | cut -d: -f2 | tr -d '[:space:]')
fps=""
[ -n "$vb" ] && fps=$(awk -v vb="$vb" 'BEGIN{ t=vb+2192; if(t>0) printf "%.2f", 135000.0/t }')

echo "{"
echo "  \"sensor\": \"imx415\","
echo "  \"video_node\": \"$VID\","
echo "  \"subdev\": \"$SUB\","
echo "  \"vertical_blanking\": ${vb:-null},"
echo "  \"derived_fps\": ${fps:-null},"
echo "  \"controls\": ["
{ emit "$VID" isp; emit "$SUB" sensor; } | sed '$ s/,$//'
echo "  ]"
echo "}"
