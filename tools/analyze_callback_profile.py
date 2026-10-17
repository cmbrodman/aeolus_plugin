#!/usr/bin/env python3
"""Summarize opt-in Aeolus processor timing; this is not the full device callback."""
import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values, percent):
    ordered = sorted(values)
    position = (len(ordered)-1)*percent/100
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high]-ordered[low])*(position-low)


def summarize(path, voices=None, warmup=0):
    lines = Path(path).read_text().splitlines()
    metadata = dict(line[2:].split('=', 1) for line in lines if line.startswith('# ') and '=' in line)
    rows = list(csv.DictReader(line for line in lines if not line.startswith('#')))
    if not rows:
        raise ValueError('No timing records')
    first_start = int(rows[0]['start_ns'])
    valid = [r for r in rows if int(r['frames']) > 0 and float(r['sample_rate']) > 0
             and (int(r['start_ns'])-first_start)/1e9 >= warmup
             and (voices is None or int(r['voices']) == voices)]
    if not valid:
        raise ValueError('No records match the requested workload')
    durations = [int(r['duration_ns'])/1e6 for r in valid]
    budgets = [int(r['frames'])/float(r['sample_rate'])*1000 for r in valid]
    loads = [d/b*100 for d, b in zip(durations, budgets)]
    native = [int(r['native_xruns']) for r in valid if int(r['native_xruns']) >= 0]
    resets = sum(b<a for a,b in zip(native, native[1:]))
    observed_gaps = sum(max(0, int(b['sequence'])-int(a['sequence'])-1) for a,b in zip(rows, rows[1:]))
    return {
        'records': len(valid), 'voices': sorted({int(r['voices']) for r in valid}),
        'sample_rates': sorted({float(r['sample_rate']) for r in valid}),
        'block_sizes': sorted({int(r['frames']) for r in valid}),
        'processor_ms': {'p50':percentile(durations,50), 'p99':percentile(durations,99), 'worst':max(durations)},
        'processor_budget_percent': {'p50':percentile(loads,50), 'p99':percentile(loads,99), 'worst':max(loads)},
        'processor_budget_exceeded':sum(d>b for d,b in zip(durations,budgets)),
        'native_xruns_delta_across_selected_span':sum(max(0,b-a) for a,b in zip(native,native[1:])) if native else None,
        'native_counter_resets':resets,
        'observed_telemetry_sequence_gaps_all_records':observed_gaps,
        'reported_dropped_total':int(metadata['dropped_total']) if 'dropped_total' in metadata else None,
        'capture_closed_cleanly':'dropped_total' in metadata,
        'block_delay':metadata.get('block_delay'),
    }


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv',type=Path)
    parser.add_argument('--voices',type=int)
    parser.add_argument('--warmup-seconds',type=float,default=5)
    args=parser.parse_args()
    print(json.dumps(summarize(args.csv,args.voices,args.warmup_seconds),indent=2))


if __name__=='__main__':
    main()
