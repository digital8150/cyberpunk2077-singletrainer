"""Summarize F8 shot traces; timing proximity is NOT a shot/pellet identifier.

Usage: python tools/scripts/shot_trace_report.py [trainer.log] [--json output.json]
"""
import argparse
import collections
import json
import os
from pathlib import Path


def summarize(text):
    captures = collections.defaultdict(list)
    for line in text.splitlines():
        if not line.startswith('[SHOTTRACE] cap='):
            continue
        if not all(f'{key}=' in line for key in ('cap', 'seq', 'kind', 'tid', 'qpc', 'end', 'caller',
                                                'object', 'context', 'event', 'id', 'flags', 'origin', 'before', 'after')):
            continue  # A concurrently read last line can still be incomplete.
        try:
            fields = dict(item.split('=', 1) for item in line.split()[1:])
            for key in ('cap', 'seq', 'kind', 'tid', 'qpc', 'end', 'flags'):
                fields[key] = int(fields[key])
            for key in ('caller', 'object', 'context', 'event', 'id'):
                fields[key] = int(fields[key], 16)
            for key in ('origin', 'before', 'after'):
                fields[key] = [float(value) for value in fields[key].split(',')]
            if any(len(fields[key]) != 3 for key in ("origin", "before", "after")):
                continue
        except (ValueError, KeyError):
            continue
        captures[fields['cap']].append(fields)
    result = []
    # Capture numbering restarts on reinjection. Partition repeated STARTs in append-only logs.
    for cap, combined in captures.items():
        combined.sort(key=lambda r: (r['qpc'], r['seq']))
        runs = []
        current = []
        for record in combined:
            if current and record['kind'] == 0:
                # Writer ring order is not chronological. A new start is a new session only if QPC advanced.
                previous = next((r for r in current if r['kind'] == 0), None)
                if previous and record['qpc'] > previous['qpc']:
                    runs.append(current)
                    current = []
            current.append(record)
        if current:
            runs.append(current)
        for records in runs:
            records.sort(key=lambda r: (r['qpc'], r['seq']))
            start = next((r for r in records if r['kind'] == 0), None)
            stop = next((r for r in reversed(records) if r['kind'] == 1), None)
            if not start:
                result.append({'capture': cap, 'error': 'Missing START; cannot convert QPC or establish completeness'})
                continue
            frequency = start['id']
            if frequency <= 0:
                result.append({'capture': cap, 'error': 'Invalid QPC frequency'})
                continue
            ms = lambda r: round((r['qpc'] - start['qpc']) * 1000 / frequency, 3)
            calls = [r for r in records if r['kind'] == 4]
            caller_groups = collections.defaultdict(list)
            for r in calls:
                caller_groups[(r['caller'], r['tid'])].append(r)
            caller_stats = []
            for (caller, thread), group in caller_groups.items():
                gaps = [(b['qpc'] - a['qpc']) * 1000 / frequency for a, b in zip(group, group[1:])]
                caller_stats.append({'address': hex(caller), 'offset_from_game_base': hex(caller - start['object']),
                                     'thread': thread, 'calls': len(group),
                                     'redirects': sum(bool(r['flags'] & 2) for r in group),
                                     'min_gap_ms': round(min(gaps), 4) if gaps else None})
            events = [r for r in records if r['kind'] == 5]
            weapons = [r for r in records if r['kind'] == 3]
            known_weapons = {r['object'] for r in weapons if r['object']}
            result.append({
                'capture': cap, 'start_qpc': start['qpc'], 'complete_stop': stop is not None,
                'dropped': max(0, stop['id'] - start['event']) if stop else None,
                'duration_ms': ms(stop) if stop else ms(records[-1]),
                'crosshair_calls': len(calls), 'callers': caller_stats,
                'input_edges': [{'ms': ms(r), 'buttons': r['flags'] & 3,
                                 'aim_enabled': bool(r['flags'] & 256), 'silent': bool(r['flags'] & 512),
                                 'no_spread': bool(r['flags'] & 1024), 'no_recoil': bool(r['flags'] & 2048),
                                 'bone_mask': (r['flags'] >> 12) & 31,
                                 'nearest': bool(r['flags'] & (1 << 17)),
                                 'profile': ((r['flags'] >> 18) & 1) + 1} for r in records if r['kind'] == 2],
                'weapon_samples': [{'ms': ms(r), 'entity_id': hex(r['id']), 'object': hex(r['object']),
                                    'projectiles_per_shot': r['origin'][0], 'status': r['flags']} for r in weapons],
                'queued_events': [{'ms': ms(r), 'event_type_id': r['flags'], 'event_pointer': hex(r['event']),
                                   'receiver': hex(r['object']), 'thread': r['tid'],
                                   'receiver_matches_sampled_weapon': r['object'] in known_weapons} for r in events],
                'limitations': 'Queue entries may be duplicate dispatches or NPC events. Input edges are frame-sampled. '
                               'Receiver pointer matches are identity hints, not retained ownership. '
                               'Call frequency and ProjectilesPerShot do not establish pellet boundaries.',
            })
    return sorted(result, key=lambda report: report.get('start_qpc', 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', nargs='?', type=Path,
                        default=Path(os.environ.get('LOCALAPPDATA', '.')) / 'cp2077_trainer/cp2077_trainer.log')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    reports = summarize(args.log.read_text(encoding='utf-8-sig', errors='replace'))
    if args.json:
        args.json.write_text(json.dumps(reports, indent=2), encoding='utf-8')
    if not reports:
        print('No SHOTTRACE captures. Close the menu and press F8 in game to record 20 seconds.')
    for report in reports[-5:]:
        if 'error' in report:
            print(report)
            continue
        print(f"Capture {report['capture']}: {report['duration_ms']} ms, "
              f"crosshair={report['crosshair_calls']}, queued={len(report['queued_events'])}, "
              f"dropped={report['dropped']}, stopped={report['complete_stop']}")
        for caller in report['callers']:
            print('  caller:', caller)
        print('  projectiles/shot samples:', sorted({r['projectiles_per_shot'] for r in report['weapon_samples']}))
        print('  NOTE:', report['limitations'])


if __name__ == '__main__':
    main()
