#!/usr/bin/env python3
"""Run screenshot batches from a macOS desktop terminal; accept capture jobs only."""
import argparse
import fcntl
import json
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=Path.home() / 'harbor-build/Harbor.app')
    parser.add_argument('--out', type=Path, default=ROOT / '.shots')
    parser.add_argument('--idle-minutes', type=int, default=45)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('The desktop runner supports macOS only')
    if args.idle_minutes <= 0:
        parser.error('--idle-minutes must be positive')
    args.app = args.app.expanduser().resolve()
    args.out = args.out.expanduser().resolve()
    binary=args.app/'Contents/MacOS/Harbor' if args.app.suffix=='.app' else args.app
    if not binary.is_file():
        parser.error('APP path must be a Harbor executable or .app bundle: '+str(args.app))
    control = args.out.resolve() / 'control'
    control.mkdir(parents=True, exist_ok=True)
    runner_lock = (control / '.desktop.lock').open('a')
    try:
        fcntl.flock(runner_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('A desktop runner already owns this output directory')
    (control / 'ready.json').write_text(json.dumps({'ready': True, 'app': str(args.app)}))
    print('macOS screenshot runner ready. Ctrl-C stops it. No shell commands are accepted.', flush=True)
    deadline = time.monotonic() + args.idle_minutes * 60
    try:
        while time.monotonic() < deadline:
            if (control / 'stop').exists():
                (control / 'stop').unlink()
                break
            for request in sorted(control.glob('request-*.json')):
                running = request.with_suffix('.running')
                request.replace(running)
                result_path = request.with_name(request.stem.replace('request-', 'result-') + '.json')
                try:
                    job = json.loads(running.read_text())
                    if not isinstance(job, dict):
                        raise ValueError('Job must be an object containing scene IDs')
                    ids = job.get('ids', [])
                    if not isinstance(ids, list) or any(not isinstance(x, str) or x not in {f'SM-{i:02d}' for i in range(1,136)} for x in ids):
                        raise ValueError('Invalid scene IDs')
                    if set(job) - {'ids'}:
                        raise ValueError('Only the ids field is supported')
                    command = [sys.executable, str(HERE / 'capture.py'), 'app', '--foreground',
                               '--app', str(args.app), '--out', str(args.out), *ids]
                    with result_path.with_suffix('.log').open('w') as log:
                        process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                    result = {'exit_code': process.returncode, 'ids': ids}
                except Exception as error:
                    result = {'error': str(error)}
                pending = result_path.with_suffix('.tmp')
                pending.write_text(json.dumps(result, ensure_ascii=False, indent=2))
                pending.replace(result_path)
                running.unlink()
                deadline = time.monotonic() + args.idle_minutes * 60
            time.sleep(.25)
    finally:
        (control / 'ready.json').unlink(missing_ok=True)
        runner_lock.close()


if __name__ == '__main__':
    main()
