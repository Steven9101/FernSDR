import contextlib
import io
import json
import unittest
from unittest.mock import MagicMock, patch

from isolation import helper


class ProbeTests(unittest.TestCase):
    def test_successful_handshake_remains_connected_when_peer_stalls(self):
        connection = MagicMock()
        connection.__enter__.return_value = connection
        connection.recv.side_effect = TimeoutError()
        output = io.StringIO()
        with patch('isolation.socket.create_connection', return_value=connection), contextlib.redirect_stdout(output):
            helper('probe', '127.0.0.1', '1', 'nonce')
        self.assertTrue(json.loads(output.getvalue())['connected'])
        self.assertFalse(json.loads(output.getvalue())['nonce_ok'])

    def test_failed_handshake_is_distinguished(self):
        output = io.StringIO()
        with patch('isolation.socket.create_connection', side_effect=TimeoutError()), contextlib.redirect_stdout(output):
            helper('probe', '127.0.0.1', '1', 'nonce')
        self.assertFalse(json.loads(output.getvalue())['connected'])


if __name__ == '__main__':
    unittest.main()
