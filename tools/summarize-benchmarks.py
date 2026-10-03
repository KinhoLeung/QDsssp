#!/usr/bin/env python3
"""Summarize the logs produced by run-board-comparison.sh."""
import argparse
import json
from pathlib import Path
from statistics import median


def read_log(path):
    scenario, version, repeat = path.stem.rsplit('-', 2)
    record = {'scenario': scenario, 'version': version, 'repeat': int(repeat)}
    for line in path.read_text().splitlines():
        if line.startswith('Qt '):
            parts = line.split(', ')
            record['metadata'] = dict(item.split('=', 1) for item in parts[1:])
            record['metadata']['qt'] = parts[0]
        elif line.startswith(('warmup_updates=', 'process_cpu_', 'pcm_input_samples=')):
            for item in line.split(', '):
                key, value = item.split('=', 1)
                record[key] = float(value)
        elif ' mean/p50/p95/p99/max_ms=' in line:
            name, rest = line.split(' count=', 1)
            count, rest = rest.split(', mean/p50/p95/p99/max_ms=', 1)
            values, sampled = rest.split(', sampled=')
            record[name] = dict(zip(('mean', 'p50', 'p95', 'p99', 'max'), map(float, values.split('/'))))
            record[name].update(count=int(count), sampled=int(sampled))
    if 'metadata' not in record or 'process_cpu_percent_one_core' not in record:
        raise ValueError(f'Incomplete benchmark: {path}')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path)
    parser.add_argument('--output', type=Path, help='Write all samples and median summaries as JSON.')
    args = parser.parse_args()
    rows = [read_log(path) for path in sorted(args.logs.glob('*-baseline-*.log'))]
    rows += [read_log(path) for path in sorted(args.logs.glob('*-optimized-*.log'))]
    if not rows:
        parser.error('No benchmark logs found.')
    summaries = []
    print('| Scene | Version | Runs | CPU % | Paint fps | Prepare P95 ms | Paint mean / P95 ms | FFT mean ms |')
    print('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |')
    for scenario in sorted({row['scenario'] for row in rows}):
        for repeat in {row['repeat'] for row in rows if row['scenario'] == scenario}:
            pair = [row for row in rows if row['scenario'] == scenario and row['repeat'] == repeat]
            if len(pair) != 2 or pair[0]['metadata'] != pair[1]['metadata']:
                raise ValueError(f'Unmatched benchmark parameters: {scenario}, repeat {repeat}')
        for version in ('baseline', 'optimized'):
            group = [row for row in rows if row['scenario'] == scenario and row['version'] == version]
            summary = {'scenario': scenario, 'version': version, 'runs': len(group)}
            for field in ('process_cpu_percent_one_core', 'paint_fps', 'peak_rss_kib', 'pcm_fft_mean_ms'):
                if field in group[0]:
                    summary[field] = median(row[field] for row in group)
            for phase in ('prepare', 'response', 'paths', 'overlay', 'curves', 'controls', 'spectrum', 'paint', 'paint_interval', 'footer'):
                summary[phase] = {key: median(row[phase][key] for row in group) for key in ('mean', 'p50', 'p95', 'p99', 'max')}
            summary['dropped_samples'] = sum(row.get('pcm_dropped_samples', 0) for row in group)
            summaries.append(summary)
            fft = f'{summary["pcm_fft_mean_ms"]:.3f}' if 'pcm_fft_mean_ms' in summary else '—'
            print(f'| {scenario} | {version} | {len(group)} | {summary["process_cpu_percent_one_core"]:.3f} | '
                  f'{summary["paint_fps"]:.3f} | {summary["prepare"]["p95"]:.3f} | '
                  f'{summary["paint"]["mean"]:.3f} / {summary["paint"]["p95"]:.3f} | {fft} |')
    if args.output:
        args.output.write_text(json.dumps({'runs': rows, 'medians': summaries}, indent=2))


if __name__ == '__main__':
    main()
