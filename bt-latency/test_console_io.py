import unittest
from unittest.mock import patch
import console_io

class ConsoleWriteTests(unittest.TestCase):
    def test_short_write_and_retry_preserve_every_byte(self):
        accepted=bytearray();calls=0
        def write(fd,data):
            nonlocal calls
            calls+=1
            if calls==2:raise BlockingIOError()
            size=min(3,len(data));accepted.extend(data[:size]);return size
        payload=bytes(range(100))+b"\\r"
        with patch.object(console_io.os,"write",write), patch.object(console_io.select,"select",return_value=([],[7],[])), patch.object(console_io.time,"sleep"):
            console_io.write_paced(7,payload)
        self.assertEqual(accepted,payload)
    def test_zero_write_fails(self):
        with patch.object(console_io.os,"write",return_value=0), patch.object(console_io.select,"select",return_value=([],[7],[])):
            with self.assertRaises(OSError):console_io.write_paced(7,b"x")
    def test_timeout_is_bounded(self):
        with patch.object(console_io.time,"monotonic",side_effect=[0,11]):
            with self.assertRaises(TimeoutError):console_io.write_paced(7,b"x")

if __name__=="__main__":unittest.main()
