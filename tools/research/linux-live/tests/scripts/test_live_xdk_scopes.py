import importlib.util
from pathlib import Path
import unittest
import json
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FOLDER = ROOT / 'scripts'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class ScopeProbeTests(unittest.TestCase):


    def test_nested_scopes_partition_and_same_caller_queue_overlap(self):
        m = module('analysis', FOLDER / 'analyze-xemu-live-xdk-scopes.py')
        def row(seq, ns, tid, event, id_, a=0, b=0):
            return dict(seq=seq, ns=ns, tid=tid, event=event, id=id_, a=a, b=b)
        rows = [row(1,100,7,58,1,0x351090,1000),row(2,110,7,58,2,0x351770,990),
                row(3,120,7,1,3,44),row(4,130,9,2,3,44),row(5,135,9,3,3,44),
                row(6,138,7,4,3,44),row(7,140,7,59,2,0x351770),row(8,150,7,59,1,0x351090)]
        out=m.analyze(rows,90,160)
        self.assertEqual(out['scope_union_ns'],50)
        self.assertEqual(out['by_va']['0x351090']['exclusive_ns'],20)
        self.assertEqual(out['by_va']['0x351770']['exclusive_ns'],30)
        self.assertEqual(out['scope_union_queue_ns'],10)
        self.assertEqual(out['by_va']['0x351770']['exclusive_queue_ns'],10)
        self.assertEqual(out['scope_union_own_service_ns'],5)

    def test_wrong_tid_or_non_lifo_end_is_rejected_and_edges_censored(self):
        m = module('analysis', FOLDER / 'analyze-xemu-live-xdk-scopes.py')
        def r(seq,event,id_,tid=7,va=0x351090):
            return dict(seq=seq,ns=100+seq,tid=tid,event=event,id=id_,a=va,b=0)
        with self.assertRaises(ValueError):m.analyze([r(1,58,1),r(2,59,1,8)],90,160)
        with self.assertRaises(ValueError):m.analyze([r(1,58,1),r(2,58,2,va=0x351770),r(3,59,1)],90,160)
        out=m.analyze([r(1,59,0),r(2,58,2)],90,160)
        self.assertEqual(out['scope_union_ns'],0)
        self.assertEqual(len(out['censored_scopes']),2)

    def test_real_cli_validates_header_and_accepts_new_scope_events(self):
        header=[0x4233545241434531,1,56,0x0102030405060708,90,160,100,1]
        rows=[(1,100,7,58,1,0x351090,1000),(2,150,7,59,1,0x351090,42)]
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'trace.bin'
            path.write_bytes(struct.pack('<8Q',*header)+b''.join(struct.pack('<7Q',*r) for r in rows))
            result=subprocess.run([sys.executable,str(FOLDER/'analyze-xemu-live-xdk-scopes.py'),str(path)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads(result.stdout)['scope_union_ns'],50)
            header[7]=0
            path.write_bytes(struct.pack('<8Q',*header))
            result=subprocess.run([sys.executable,str(FOLDER/'analyze-xemu-live-xdk-scopes.py'),str(path)],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)


if __name__ == '__main__':unittest.main()
