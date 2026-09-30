"""Check the recorded prefix hash against bytes received by an independent reader."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class EvidenceTests(unittest.TestCase):
    def test_prefix_hash_and_seed_repeatability(self):
        root = Path(__file__).resolve().parents[1]
        hashes = []
        with tempfile.TemporaryDirectory() as directory:
            for index, seed in enumerate((1, 1, 2)):
                evidence = Path(directory) / f'{index}.json'
                child = subprocess.Popen([sys.executable, str(root / 'source/scene.py'),
                    str(root / 'scenes/quiet.scene'), '--seed', str(seed), '--seconds', '17',
                    '--evidence', str(evidence)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                digest = hashlib.sha256()
                total = 0
                try:
                    while block := child.stdout.read(65536):
                        if total < 64 * 1024 * 1024:
                            digest.update(block[:64 * 1024 * 1024 - total])
                        total += len(block)
                    _, error = child.communicate(timeout=10)
                    self.assertEqual(child.returncode, 0, error.decode())
                    self.assertEqual(total, 17 * 2048000 * 2)
                    proof = json.loads(evidence.read_text())
                    self.assertEqual(proof['bytes'], 64 * 1024 * 1024)
                    self.assertEqual(proof['sha256'], digest.hexdigest())
                    self.assertEqual(proof['seed'], seed)
                    hashes.append(proof['sha256'])
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.communicate(timeout=3)
        self.assertEqual(hashes[0], hashes[1])
        self.assertNotEqual(hashes[1], hashes[2])


if __name__ == '__main__':
    unittest.main()
