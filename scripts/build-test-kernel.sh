#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$project_dir/vendor/linux-v6.18.44"
output_dir="$project_dir/build/linux-v6.18.44-imx415"
jobs=${JOBS:-2}

make -C "$source_dir" O="$output_dir" -j"$jobs" Image
