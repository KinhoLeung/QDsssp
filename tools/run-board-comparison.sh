#!/bin/sh
set -eu

# Run inside the extracted ARM comparison bundle. Keep each version's workload
# and Qt platform identical; alternate order to reduce warm-up/order bias.
benchmark_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
benchmark_fps=${QDSSSP_BENCHMARK_FPS:-30}
benchmark_updates=${QDSSSP_BENCHMARK_UPDATES:-300}
benchmark_warmup=${QDSSSP_BENCHMARK_WARMUP:-60}
for benchmark_number in "$benchmark_fps" "$benchmark_updates" "$benchmark_warmup"; do
    case "$benchmark_number" in
        ''|*[!0-9]*) echo 'Benchmark settings must be nonnegative integers.' >&2; exit 2 ;;
    esac
done
if [ "$benchmark_fps" -lt 10 ] || [ "$benchmark_fps" -gt 60 ] || [ "$benchmark_updates" -lt 1 ]; then
    echo 'Use 10..60 fps and at least one measured update.' >&2
    exit 2
fi
for benchmark_file in qdsssp_baseline qdsssp_optimized qdsssp_tests DOGFIGHT.pcm; do
    if [ ! -r "$benchmark_root/$benchmark_file" ]; then
        echo "Missing comparison file: $benchmark_file" >&2
        exit 2
    fi
done
benchmark_platform=${QT_QPA_PLATFORM:-linuxfb}
if [ -d /opt/qt5.15.19-arm/lib ]; then
    export LD_LIBRARY_PATH="/opt/qt5.15.19-arm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [ -d "$benchmark_root/lib" ]; then
    export LD_LIBRARY_PATH="$benchmark_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
benchmark_logs=$(mktemp -d "$benchmark_root/results-${benchmark_fps}fps-XXXXXX")
{
    echo "platform=$benchmark_platform, fps=$benchmark_fps, measured_updates=$benchmark_updates, warmup_updates=$benchmark_warmup"
    uname -a
    cat /proc/cpuinfo
    grep Cpus_allowed_list "/proc/$$/status"
    cat /proc/meminfo
    for benchmark_sysfile in /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq; do
        if [ -r "$benchmark_sysfile" ]; then
            echo "$benchmark_sysfile"
            cat "$benchmark_sysfile"
        fi
    done
    if [ -r /sys/kernel/debug/clk/clk_summary ]; then
        grep -E 'armclk|clk_core_pvtpll' /sys/kernel/debug/clk/clk_summary
    fi
    sha256sum "$benchmark_root/qdsssp_baseline" "$benchmark_root/qdsssp_optimized" "$benchmark_root/DOGFIGHT.pcm"
} > "$benchmark_logs/environment.log"
if ! "$benchmark_root/qdsssp_tests" -platform offscreen > "$benchmark_logs/tests.log" 2>&1; then
    cat "$benchmark_logs/tests.log"
    exit 1
fi
echo "QtTest passed; logs: $benchmark_logs"
for benchmark_repeat in 1 2 3; do
    benchmark_versions='baseline optimized'
    if [ "$benchmark_repeat" -eq 2 ]; then benchmark_versions='optimized baseline'; fi
    for benchmark_scenario in synthetic-dynamic synthetic-static pcm-dynamic pcm-static eq-only synthetic-burst; do
        for benchmark_version in $benchmark_versions; do
            set -- -platform "$benchmark_platform" --benchmark "$benchmark_updates" --warmup "$benchmark_warmup" --fps "$benchmark_fps"
            case "$benchmark_scenario" in
                synthetic-static) set -- "$@" --static-eq ;;
                pcm-dynamic) set -- "$@" --benchmark-pcm --pcm-path "$benchmark_root/DOGFIGHT.pcm" ;;
                pcm-static) set -- "$@" --benchmark-pcm --pcm-path "$benchmark_root/DOGFIGHT.pcm" --static-eq ;;
                eq-only) set -- "$@" --benchmark-no-spectrum ;;
                synthetic-burst) set -- "$@" --benchmark-burst ;;
            esac
            benchmark_log="$benchmark_logs/$benchmark_scenario-$benchmark_version-$benchmark_repeat.log"
            echo "Running $benchmark_scenario / $benchmark_version / repeat $benchmark_repeat"
            if ! "$benchmark_root/qdsssp_$benchmark_version" "$@" > "$benchmark_log" 2>&1; then
                cat "$benchmark_log"
                exit 1
            fi
            sed -n '/^warmup_updates=/p; /^process_cpu_/p; /^pcm_input_samples=/p' "$benchmark_log"
        done
    done
done
echo "Completed 36 comparisons; logs: $benchmark_logs"
