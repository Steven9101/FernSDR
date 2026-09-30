"""Exercise isolation in an owned lab using canaries, without a receiver image."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'harness'))
from lab import LabSession, Client, sh, LAB_NET
from isolation import check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    with LabSession():
        client = Client(1)
        name = 'fbcanary-' + uuid.uuid4().hex[:8]
        process = None
        owned = False
        try:
            sh('ip', 'netns', 'add', name)
            owned = True
            process = subprocess.Popen(['ip', 'netns', 'exec', name, 'sleep', '60'])
            time.sleep(.1)
            sh(LAB_NET, 'receiver', str(process.pid))
            result = check(process.pid)
            (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps({'ok': result['ok'], 'cases': len(result['cases']), 'scope': result['scope']}))
        finally:
            try:
                if process:
                    process.terminate()
                    process.wait(timeout=3)
            finally:
                try:
                    if owned:
                        sh('ip', 'netns', 'del', name)
                finally:
                    client.close()


if __name__ == '__main__':
    main()
