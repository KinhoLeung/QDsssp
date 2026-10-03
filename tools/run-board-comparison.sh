#!/bin/sh
set -eu

# Run inside the extracted ARM comparison bundle. Keep each version's workload
# and Qt platform identical; alternate order to reduce warm-up/order bias.
benchmark_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
benchmark_fps=${QDSSSP_BENCHMARK_FPS:-30}
benchmark_updates=${QDSSSP_BENCHMARK_UPDATES:-300}
benchmark_warmup=${QDSSSP_BENCHMARK_WARMUP:-60}
benchmark_repeats=${QDSSSP_BENCHMARK_REPEATS:-3}
benchmark_bands=${QDSSSP_BENCHMARK_BANDS:-32}
benchmark_scenarios=${QDSSSP_BENCHMARK_SCENARIOS:-'synthetic-dynamic synthetic-static pcm-dynamic pcm-static eq-only synthetic-burst'}
for benchmark_number in "$benchmark_fps" "$benchmark_updates" "$benchmark_warmup" "$benchmark_repeats" $benchmark_bands; do
    case "$benchmark_number" in
        ''|*[!0-9]*) echo 'Benchmark settings must be nonnegative integers.' >&2; exit 2 ;;
    esac
done
if [ "$benchmark_fps" -lt 10 ] || [ "$benchmark_fps" -gt 60 ] || [ "$benchmark_updates" -lt 1 ] || [ "$benchmark_repeats" -lt 1 ]; then
    echo 'Use 10..60 fps, at least one measured update and one repeat.' >&2
    exit 2
fi
for benchmark_count in $benchmark_bands; do
    if [ "$benchmark_count" -lt 1 ] || [ "$benchmark_count" -gt 32 ]; then
        echo 'Use 1..32 bands.' >&2; exit 2
    fi
done
for benchmark_scenario in $benchmark_scenarios; do
    case "$benchmark_scenario" in
        synthetic-dynamic|synthetic-static|pcm-dynamic|pcm-static|eq-only|synthetic-burst) ;;
        *) echo "Unknown scenario: $benchmark_scenario" >&2; exit 2 ;;
    esac
done
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
    echo "platform=$benchmark_platform, fps=$benchmark_fps, measured_updates=$benchmark_updates, warmup_updates=$benchmark_warmup, bands=$benchmark_bands, repeats=$benchmark_repeats, scenarios=$benchmark_scenarios"
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
benchmark_repeat=1
benchmark_runs=0
while [ "$benchmark_repeat" -le "$benchmark_repeats" ]; do
    benchmark_versions='baseline optimized'
    if [ "$((benchmark_repeat % 2))" -eq 0 ]; then benchmark_versions='optimized baseline'; fi
    for benchmark_count in $benchmark_bands; do
    for benchmark_scenario in $benchmark_scenarios; do
        for benchmark_version in $benchmark_versions; do
            set -- -platform "$benchmark_platform" --benchmark "$benchmark_updates" --warmup "$benchmark_warmup" --fps "$benchmark_fps" --benchmark-bands "$benchmark_count"
            case "$benchmark_scenario" in
                synthetic-static) set -- "$@" --static-eq ;;
                pcm-dynamic) set -- "$@" --benchmark-pcm --pcm-path "$benchmark_root/DOGFIGHT.pcm" ;;
                pcm-static) set -- "$@" --benchmark-pcm --pcm-path "$benchmark_root/DOGFIGHT.pcm" --static-eq ;;
                eq-only) set -- "$@" --benchmark-no-spectrum ;;
                synthetic-burst) set -- "$@" --benchmark-burst ;;
            esac
            benchmark_log="$benchmark_logs/b$benchmark_count-$benchmark_scenario-$benchmark_version-$benchmark_repeat.log"
            echo "Running $benchmark_count bands / $benchmark_scenario / $benchmark_version / repeat $benchmark_repeat"
            if ! "$benchmark_root/qdsssp_$benchmark_version" "$@" > "$benchmark_log" 2>&1; then
                cat "$benchmark_log"
                exit 1
            fi
            sed -n '/^warmup_updates=/p; /^process_cpu_/p; /^pcm_input_samples=/p' "$benchmark_log"
            benchmark_runs=$((benchmark_runs + 1))
        done
    done
    done
    benchmark_repeat=$((benchmark_repeat + 1))
done
echo "Completed $benchmark_runs benchmark runs; logs: $benchmark_logs"
