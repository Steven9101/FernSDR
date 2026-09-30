import os
import subprocess
import tempfile
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

import lab


class LifecycleTests(unittest.TestCase):
    def test_timeout_stops_entire_client_scope(self):
        client = object.__new__(lab.Client)
        client.n, client.ns = 1, "fbc1"
        with patch.object(lab.subprocess, "run", side_effect=subprocess.TimeoutExpired("browser", 1)), \
                patch.object(lab, "sh") as cleanup:
            with self.assertRaises(subprocess.TimeoutExpired):
                client.run(["node", "listen.mjs"], timeout=1)
            self.assertEqual(cleanup.call_args.args[:2], ("systemctl", "stop"))
            self.assertTrue(cleanup.call_args.args[2].endswith(".scope"))

    def test_partial_pulse_start_preserves_original_failure(self):
        session = lab.LabSession()
        session.lock = Mock()
        session.pulse_unit = "fbh-pulse-test"
        session.network_owned = session.slice_owned = True
        session.old_pulse = "previous"
        with tempfile.TemporaryDirectory() as directory:
            session.pulse_dir = directory
            session.pulse_log = open(Path(directory) / "pulse.log", "w")
            with patch.object(lab, "sh"), patch.dict(os.environ, {"FB_PULSE_SERVER": "temporary"}):
                error = OSError("cannot spawn owned process")
                session.__exit__(OSError, error, None)
                self.assertEqual(os.environ["FB_PULSE_SERVER"], "previous")
            self.assertTrue(session.pulse_log.closed)
            self.assertFalse(Path(directory).exists())
            session.lock.close.assert_called_once()

    def test_cleanup_failure_does_not_skip_remaining_owned_resources(self):
        session = lab.LabSession()
        session.lock = Mock()
        session.pulse_unit = "fbh-pulse-test"
        session.pulse = Mock()
        session.pulse.wait.side_effect = subprocess.TimeoutExpired("pulse", 10)
        session.network_owned = session.slice_owned = True
        session.old_pulse = None
        with tempfile.TemporaryDirectory() as directory:
            session.pulse_dir = directory
            session.pulse_log = open(Path(directory) / "pulse.log", "w")
            with patch.object(lab, "sh") as cleanup, patch.dict(os.environ, {"FB_PULSE_SERVER": "temporary"}):
                with self.assertRaisesRegex(RuntimeError, "cleanup"):
                    session.__exit__(None, None, None)
                self.assertNotIn("FB_PULSE_SERVER", os.environ)
            self.assertTrue(session.pulse_log.closed)
            self.assertFalse(Path(directory).exists())
            self.assertIn((lab.LAB_NET, "down"), [c.args for c in cleanup.call_args_list])
            self.assertIn(("systemctl", "stop", lab.SLICE), [c.args for c in cleanup.call_args_list])
            session.lock.close.assert_called_once()

    def test_missing_cgroup_is_not_zero_cpu(self):
        sampler = lab.CgroupSampler(["/does-not-exist"])
        with self.assertRaisesRegex(RuntimeError, "cannot sample"):
            sampler.sample()

    def test_manifest_id_cannot_escape_receiver_directory(self):
        with self.assertRaisesRegex(ValueError, "receiver id"):
            lab.load_receiver("../../server")

    def test_cleanup_refuses_container_with_another_owner(self):
        rx = lab.ReceiverRun("fernsdr", "documented", "/tmp/nonexistent-bench-test", "markers", 1)
        rx.owned_containers = [rx.name]
        with patch.object(lab, "sh", return_value=subprocess.CompletedProcess([], 0, "another-owner\n", "")) as sh:
            rx.stop()
            self.assertFalse(any(call.args[:3] == ("docker", "rm", "-f") for call in sh.call_args_list))


if __name__ == "__main__":
    unittest.main()
