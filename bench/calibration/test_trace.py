"""Known timeline fixtures independent of the PulseAudio trace producer."""
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
from analyse_trace import segments, pulse_times


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.events = [{'ev': 'start', 'pulse_version': '16.1', 'rate': 48000,
                        'channels': 2, 'format': 'float32le'}]
        self.pcm = []
        self.offset = 0

    def event(self, kind, frame, audio=None):
        data = np.repeat(np.asarray(audio if audio is not None else [], dtype='<f4'), 2)
        self.events.append({'ev': kind, 't0_mono_ns': 1_000_000_000 + round(frame * 1e9 / 48000),
                            'bracket_ns': 1000, 'mono_ns': 1_000_000_000,
                            'real_ns': 11_000_000_000, 'bytes': data.nbytes,
                            'file_offset': self.offset})
        self.pcm.extend(data)
        self.offset += data.nbytes

    def save(self, dropped=0):
        (self.directory / 'sink.jsonl').write_text('\n'.join(map(json.dumps,
            self.events + [{'ev': 'end', 'dropped': dropped}])) + '\n')
        np.asarray(self.pcm, dtype='<f4').tofile(self.directory / 'sink.f32')

    def test_rewind_removes_old_future_pulse(self):
        self.event('render', 0, [0] * 48 + [.25] * 48)
        self.event('rewind', 24)
        self.event('render', 24, [0] * 48 + [.25] * 24)
        self.save()
        starts, uncertainty = pulse_times(self.directory)
        self.assertEqual(len(starts), 1)
        self.assertAlmostEqual(starts[0], 11 + 72 / 48000, places=8)
        self.assertEqual(uncertainty, 1)

    def test_chunk_boundary_does_not_duplicate_pulse(self):
        self.event('render', 0, [0] * 48 + [.25] * 48)
        self.event('render', 96, [.25] * 48 + [0] * 48)
        self.save()
        self.assertEqual(len(pulse_times(self.directory)[0]), 1)

    def test_overflow_rejected(self):
        self.event('render', 0, [0] * 48)
        self.save(dropped=1)
        with self.assertRaisesRegex(ValueError, 'overflowed'):
            segments(self.directory)

    def test_excessive_observation_bracket_rejected(self):
        self.event('render', 0, [0] * 48)
        self.events[-1]['bracket_ns'] = 50_001
        self.save()
        with self.assertRaisesRegex(ValueError, 'bracket'):
            segments(self.directory)

    def test_negative_bracket_rejected(self):
        self.event('render', 0, [0] * 48)
        self.events[-1]['bracket_ns'] = -1
        self.save()
        with self.assertRaisesRegex(ValueError, 'bracket'):
            segments(self.directory)

    def test_reused_file_offset_rejected(self):
        self.event('render', 0, [0] * 48)
        self.event('render', 48, [.25] * 48)
        self.events[-1]['file_offset'] = 0
        self.save()
        with self.assertRaisesRegex(ValueError, 'offset'):
            segments(self.directory)

    def test_unaccounted_pcm_rejected(self):
        self.event('render', 0, [0] * 48)
        self.pcm.append(1)
        self.save()
        with self.assertRaisesRegex(ValueError, 'PCM'):
            segments(self.directory)

    def test_nonfinite_pcm_rejected(self):
        self.event('render', 0, [float('nan')] * 48)
        self.save()
        with self.assertRaisesRegex(ValueError, 'finite'):
            segments(self.directory)

    def test_realtime_jump_rejected(self):
        self.event('render', 0, [0] * 48)
        self.event('render', 48, [.25] * 48)
        self.events[-1]['real_ns'] += 7_000_000
        self.save()
        with self.assertRaisesRegex(ValueError, 'clock'):
            segments(self.directory)

    def test_empty_trace_rejected(self):
        self.save()
        with self.assertRaisesRegex(ValueError, 'empty'):
            segments(self.directory)


if __name__ == '__main__':
    unittest.main()
