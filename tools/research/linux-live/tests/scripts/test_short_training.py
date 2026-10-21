"""Integration checks for a disposable asset overlay; original asset remains private."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SCRIPT=ROOT/'scripts/prepare-short-training.py'
ASSET=Path(os.environ.get('B3_TRAINING_ASSET', str(ROOT/'missing-private-training-asset')))

@unittest.skipUnless(ASSET.exists(),'private original training asset not extracted')
class ShortTraining(unittest.TestCase):
    def setUp(self):
        (ROOT/'out').mkdir(exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=ROOT/'out')
        self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name)
        self.source=self.base/'source'; (self.source/'ovid').mkdir(parents=True)
        self.original=ASSET.read_bytes()
        (self.source/'ovid/TrD_ENG.xmv').write_bytes(self.original)
        (self.source/'ovid/other.xmv').write_bytes(b'untouched')
        (self.source/'default.xbe').write_bytes(b'untouched executable placeholder')
        self.output=self.base/'overlay'

    def run_prepare(self):
        return subprocess.run([sys.executable,str(SCRIPT),'--source',str(self.source),
                               '--output',str(self.output)],capture_output=True,text=True)

    def test_complete_packet_eof_decode_and_source_unchanged(self):
        p=self.run_prepare();self.assertEqual(p.returncode,0,p.stderr)
        b=(self.output/'ovid/TrD_ENG.xmv').read_bytes()
        self.assertEqual(len(b),102400)
        self.assertEqual(struct.unpack_from('<I',b,0)[0],0)
        self.assertEqual(struct.unpack_from('<I',b,36)[0],0)
        self.assertEqual(struct.unpack_from('<I',b,28)[0],200)
        self.assertEqual((self.source/'ovid/TrD_ENG.xmv').read_bytes(),self.original)
        self.assertTrue((self.output/'ovid/other.xmv').is_symlink())
        self.assertTrue((self.output/'default.xbe').is_symlink())
        result=subprocess.run(['ffprobe','-v','error','-count_frames','-show_entries',
          'stream=width,height,nb_read_frames,duration','-of','json',str(self.output/'ovid/TrD_ENG.xmv')],
          capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr,'')
        stream=json.loads(result.stdout)['streams'][0]
        self.assertEqual((stream['width'],stream['height'],stream['nb_read_frames']), (640,352,'6'))
        self.assertEqual(stream['duration'],'0.200000')

    def test_existing_output_is_untouched(self):
        self.output.mkdir();sentinel=self.output/'keep';sentinel.write_bytes(b'do not replace')
        result=self.run_prepare();self.assertNotEqual(result.returncode,0)
        self.assertIn('output already exists',result.stderr)
        self.assertEqual(sentinel.read_bytes(),b'do not replace')
        self.assertEqual(list(self.output.iterdir()),[sentinel])

    def test_modified_asset_rejected_before_creating_output(self):
        original=self.source/'ovid/TrD_ENG.xmv'
        original.write_bytes(self.original[:-1]+bytes([self.original[-1]^1]))
        result=self.run_prepare();self.assertNotEqual(result.returncode,0)
        self.assertIn('source SHA256 mismatch',result.stderr)
        self.assertFalse(self.output.exists())

if __name__=='__main__':unittest.main()
