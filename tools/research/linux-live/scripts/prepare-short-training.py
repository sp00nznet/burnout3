#!/usr/bin/env python3
"""Create a disposable game-dir overlay with a complete 200 ms training XMV.

The extracted disc and ISO are never written. The original six-frame first
packet is retained, including its WMV2 extradata; EOF and duration are explicit.
Container reference: FFmpeg libavformat/xmv.c and tools/xmv/xmv_demux.py in
the pinned local xboxrecomp checkout. Playback was verified in an experimental Linux runtime; upstream Windows playback is untested.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT=Path(__file__).resolve().parents[1]
SOURCE_SHA256='1cd0e83f03c859dbb1f0fb59bad551293af7b1056e5522b3966ad8b305e3b662'
RELATIVE_ASSET=Path('ovid/TrD_ENG.xmv')

def derive(data):
    if hashlib.sha256(data).hexdigest()!=SOURCE_SHA256:
        raise ValueError('source SHA256 mismatch: only the audited original training clip is supported')
    if len(data)!=28938240 or data[12:16]!=b'xobX':
        raise ValueError('unexpected XMV file size/magic')
    nxt,current,maximum,magic,version,width,height,duration=struct.unpack_from('<8I',data)
    audio_tracks=struct.unpack_from('<H',data,32)[0]
    packet_next,video_header,packet_end_ms=struct.unpack_from('<3I',data,36)
    frame_count=(video_header>>23)&0xff
    video_size=video_header&0x7fffff
    if (nxt,current,maximum,version,width,height,duration,audio_tracks,packet_next,
        frame_count,video_size,packet_end_ms)!=(98304,102400,110592,4,640,352,123563,0,
                                              98304,6,102352,200):
        raise ValueError('unexpected XMV header/first packet fields')
    if not video_header&0x80000000 or 48+video_size!=current:
        raise ValueError('incomplete first video packet/extradata')
    offset=52 # File header36 + packet header12 + extradata4.
    frames=[]
    for _ in range(frame_count):
        if offset+4>current:raise ValueError('truncated frame header')
        header=struct.unpack_from('<I',data,offset)[0]
        size=(header&0x1ffff)*4+4
        if offset+4+size>current:raise ValueError('truncated frame payload')
        frames.append({'offset':offset,'payload_bytes':size,'timestamp_delta_ms':header>>17})
        offset+=4+size
    shortened=bytearray(data[:current])
    struct.pack_into('<I',shortened,0,0) # File-level lookahead next size.
    struct.pack_into('<I',shortened,36,0) # First complete packet is the last.
    struct.pack_into('<I',shortened,28,packet_end_ms)
    # Verify every retained byte except these three documented header fields.
    restored=bytearray(shortened)
    for field in (0,28,36):restored[field:field+4]=data[field:field+4]
    if restored!=data[:current]:raise ValueError('unexpected payload modification')
    return bytes(shortened),{'format':'XMV','version':version,'video_codec':'WMV2',
       'width':width,'height':height,'audio_tracks':audio_tracks,
       'original_duration_ms':duration,'derived_duration_ms':packet_end_ms,
       'original_bytes':len(data),'derived_bytes':current,'frame_count':frame_count,
       'frames':frames,'frame_data_end':offset,'first_packet_padding_bytes':current-offset,
       'edited_u32_fields':{'0':0,'28':packet_end_ms,'36':0}}

def prepare(source,output):
    source=source.resolve(strict=True)
    output=output.absolute()
    if output.exists() or output.is_symlink():raise ValueError('output already exists; refusing overwrite')
    ignored=(ROOT/'out').resolve()
    resolved=output.resolve()
    if resolved==ignored or not resolved.is_relative_to(ignored):
        raise ValueError('output must be a new directory inside ignored tools/research/linux-live/out')
    if resolved.is_relative_to(source):raise ValueError('output cannot be inside the source game directory')
    original=source/RELATIVE_ASSET
    data=original.read_bytes()
    clip,fields=derive(data)
    manifest_name='short-training-manifest.json'
    if (source/manifest_name).exists():raise ValueError('source contains reserved overlay manifest filename')
    output.parent.mkdir(parents=True,exist_ok=True)
    output.mkdir() # Exclusive claim; another existing directory is never removed.
    try:
        for asset in sorted(source.iterdir()):
            if asset.name=='ovid':continue
            (output/asset.name).symlink_to(asset,target_is_directory=asset.is_dir())
        (output/'ovid').mkdir()
        for asset in sorted((source/'ovid').iterdir()):
            if asset.name=='TrD_ENG.xmv':continue
            (output/'ovid'/asset.name).symlink_to(asset,target_is_directory=asset.is_dir())
        with (output/RELATIVE_ASSET).open('xb') as handle:handle.write(clip)
        if hashlib.sha256(original.read_bytes()).hexdigest()!=SOURCE_SHA256:
            raise ValueError('source changed during preparation')
        manifest={'schema':1,'source_game_dir':str(source),'output_game_dir':str(output),
          'asset':str(RELATIVE_ASSET),'source_sha256':SOURCE_SHA256,
          'derived_sha256':hashlib.sha256(clip).hexdigest(),
          'method':'retain exact complete first packet, zero next-packet sizes, set duration to packet end',
          'fields':fields,'source_unchanged_verified':True,
          'references':['https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/xmv.c',
                        'https://github.com/sp00nznet/xboxrecomp/blob/b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4/tools/xmv/xmv_demux.py'],
          'validation_scope':'container checked during preparation; run the optional decode tests and validate playback in your runtime',
          'restore':'use the original extracted game directory; overlay is disposable',
          'benchmark_note':'navigation timing changes; take race endpoints after actual race entry'}
        with (output/manifest_name).open('x') as handle:json.dump(manifest,handle,indent=2);handle.write('\n')
    except Exception:
        shutil.rmtree(output) # Only this exclusive newly created overlay; symlinks are not followed.
        raise
    return output

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True,help='original extracted game directory')
    p.add_argument('--output',type=Path,required=True,help='new ignored overlay directory; never overwritten')
    args=p.parse_args()
    try:out=prepare(args.source,args.output)
    except (OSError,ValueError) as error:p.exit(1,f'ERROR: {error}\n')
    print(f'Prepared game-dir overlay: {out}')
    print('Training: original complete first packet, 6 frames, 200 ms; ISO and extracted source untouched.')

if __name__=='__main__':main()
