#!/usr/bin/env python3
"""Analyze bounded LIVE causal traces offline; never build or run a game."""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import unittest

MAGIC = 0x4233545241434531
MARKER = 0x0102030405060708
EVENTS = {1:'worker_enqueue',2:'worker_start',3:'worker_done',4:'worker_return',
          5:'gate_begin',6:'gate_acquired',7:'gate_released',10:'irq_level',
          11:'irq_wait_begin',12:'irq_wait_end',13:'irq_reject_gate',14:'irq_reject_mask',
          15:'irq_admit',16:'irq_leave',17:'irq_state',20:'nop',21:'isr',22:'w1c',23:'get',
          30:'d3d_entry',31:'d3d_regs',40:'batch_begin',41:'batch_submitted',42:'batch_retired',
          43:'gpu_barrier_begin',44:'gpu_barrier_end'}
STAT_NAMES = ('reserved','written','dropped','invalid','io_errors')


def distribution(values):
    """Linear-interpolated sample quantiles, with no observations kept as null."""
    if not values:
        return dict(count=0,sum=None,min=None,max=None,p50=None,p90=None,p95=None,p99=None)
    ordered=sorted(values)
    def quantile(q):
        position=(len(ordered)-1)*q
        lo=int(position);hi=min(lo+1,len(ordered)-1)
        return ordered[lo]+(ordered[hi]-ordered[lo])*(position-lo)
    return dict(count=len(ordered),sum=sum(ordered),min=ordered[0],max=ordered[-1],
                p50=quantile(.5),p90=quantile(.9),p95=quantile(.95),p99=quantile(.99))


def read_stats(log, count):
    latest=None
    if log is not None:
        for line in log.splitlines():
            if not re.search(r'\bcausal_trace\b',line):continue
            pairs=re.findall(r'\b(\w+)=([^\s]*)',line)
            if not any(name in STAT_NAMES for name,_ in pairs):continue  # startup configuration
            current={}
            for name,value in pairs:
                if name not in STAT_NAMES:continue
                if name in current or not re.fullmatch(r'\d+',value) or int(value)>0xffffffffffffffff:
                    raise ValueError('Malformed or duplicate causal_trace '+name)
                current[name]=int(value)
            if set(current)!=set(STAT_NAMES):raise ValueError('Incomplete causal_trace statistics')
            if current['written']>current['reserved']:
                raise ValueError('Trace written exceeds reserved')
            if latest and any(current[name]<latest[name] for name in STAT_NAMES):
                raise ValueError('Nonmonotonic causal_trace statistics (mixed process logs?)')
            latest=current
    reasons=[]
    if latest is None:
        status='unknown';reasons.append('No producer statistics available for this file')
    else:
        for name in ('dropped','invalid','io_errors'):
            if latest[name]:reasons.append(f'Producer reports {name}={latest[name]}')
        if latest['reserved']!=latest['written']:
            reasons.append('Reserved events exceed the flushed prefix; pending or dropped records remain')
        if latest['written']!=count:
            reasons.append('File record count differs from latest log written; file/log snapshots may differ')
        status='incomplete' if reasons else 'complete_captured_prefix'
    return dict(status=status,stats=latest,reasons=reasons,requested_window_complete=None,
                scope='Published file prefix only. Statistics do not prove the requested window has ended; missing phases remain censored even without losses.')


def decode_records(data):
    if len(data)<64:raise ValueError('Truncated 64-byte trace header')
    candidates=[(order,struct.unpack_from(order+'8Q',data)) for order in ('<','>')]
    matches=[(order,h) for order,h in candidates if h[0]==MAGIC and h[3]==MARKER]
    if len(matches)!=1:raise ValueError('Bad magic or endian marker')
    order,h=matches[0]
    _,version,width,_,begin,end,capacity,clock=h
    if version!=1 or width!=56:raise ValueError('Unsupported trace version or record width')
    if begin>=end:raise ValueError('Invalid trace event window')
    if not 1<=capacity<=1048576:raise ValueError('Invalid trace capacity')
    if clock!=1:raise ValueError('Expected Linux CLOCK_MONOTONIC (1)')
    if (len(data)-64)%width:raise ValueError('Truncated trace record; partial I/O cannot be analyzed as complete')
    count=(len(data)-64)//width
    if count>capacity:raise ValueError('Records exceed trace capacity')
    rows=[];inversions=0;previous_ns=None
    for index,words in enumerate(struct.iter_unpack(order+'7Q',data[64:]),1):
        seq,ns,tid,event,id_,a,b=words
        if seq!=index:raise ValueError(f'Noncontiguous reservation sequence at record {index}')
        if not begin<=ns<end:raise ValueError(f'Event timestamp outside half-open window at seq {seq}')
        if tid==0 or event not in EVENTS:raise ValueError(f'Invalid TID or event at seq {seq}')
        if event==1 and (id_==0 or id_!=seq):raise ValueError('Worker enqueue ID must equal its reservation sequence')
        if previous_ns is not None and ns<previous_ns:inversions+=1
        previous_ns=ns
        rows.append(dict(seq=seq,ns=ns,tid=tid,event=event,id=id_,a=a,b=b))
    return dict(version=version,record_bytes=width,byte_order='little' if order=='<' else 'big',
                begin_ns=begin,end_ns=end,requested_duration_ns=end-begin,capacity=capacity,
                clock_id=clock,clock='CLOCK_MONOTONIC'),rows,inversions


def worker_summary(rows):
    grouped=defaultdict(dict);anonymous=0
    for row in rows:
        if not 1<=row['event']<=7:continue
        if row['id']==0:anonymous+=1;continue
        phases=grouped[row['id']]
        if row['event'] in phases:raise ValueError(f'Duplicate worker phase for ID {row["id"]}')
        phases[row['event']]=row
    duration_keys={'queue':(1,2),'service':(2,3),'wake_return':(3,4),'caller_total':(1,4),
                   'gate_wait':(5,6),'gate_held':(6,7)}
    durations={key:[] for key in duration_keys};tags={};censored=[];complete=0
    for id_,phases in grouped.items():
        present=[event for event in (1,2,5,6,7,3,4) if event in phases]
        if any(phases[a]['ns']>phases[b]['ns'] for a,b in zip(present,present[1:])):
            raise ValueError(f'Causal worker timestamp inversion for ID {id_}')
        if len({p['a'] for p in phases.values()})!=1:raise ValueError(f'Worker tag changes for ID {id_}')
        worker_tids={phases[e]['tid'] for e in (2,3,5,6,7) if e in phases}
        caller_tids={phases[e]['tid'] for e in (1,4) if e in phases}
        if len(worker_tids)>1 or len(caller_tids)>1:raise ValueError(f'Worker or caller TID changes for ID {id_}')
        if 3 in phases and 4 in phases and phases[3]['b']!=phases[4]['b']:
            raise ValueError(f'Worker result changes for ID {id_}')
        tag=hex(next(iter(phases.values()))['a'])
        group=tags.setdefault(tag,{'count':0,'complete':0,'durations_ns':{key:[] for key in duration_keys}})
        group['count']+=1
        for key,(start,stop) in duration_keys.items():
            if start in phases and stop in phases:
                elapsed=phases[stop]['ns']-phases[start]['ns']
                durations[key].append(elapsed);group['durations_ns'][key].append(elapsed)
        full=all(e in phases for e in (1,2,3,4))
        if full:complete+=1;group['complete']+=1
        # Gates are optional for generic worker calls. A partial gate is still censored.
        if not full or (any(e in phases for e in (5,6,7)) and not all(e in phases for e in (5,6,7))):
            censored.append(dict(id=id_,raw_tag=tag,missing_worker_phases=[EVENTS[e] for e in (1,2,3,4) if e not in phases],
                                 missing_gate_phases=[EVENTS[e] for e in (5,6,7) if e not in phases] if any(e in phases for e in (5,6,7)) else [],
                                 observed_events=[dict(event=EVENTS[e],seq=phases[e]['seq'],ns=phases[e]['ns'],tid=phases[e]['tid']) for e in present]))
    for group in tags.values():
        group['durations_ns']={key:distribution(values) for key,values in group['durations_ns'].items()}
    return dict(chains=len(grouped),complete=complete,anonymous_events=anonymous,censored_chains=censored,
                durations_ns={key:distribution(values) for key,values in durations.items()},by_raw_tag=tags,
                scope='queue: enqueue->start; service: start->done; wake_return: done->caller return (includes scheduling/observation). Gate-held includes unlock-to-event observation. Optional gate durations are nested in service; samples and sums overlap, never a frame partition.')


def irq_summary(rows, end):
    by_tid=defaultdict(list)
    for row in rows:
        if 10<=row['event']<=17:by_tid[row['tid']].append(row)
    wait_values=[];wait_censored=[];results=dict(timeout=0,signaled=0,other=0)
    rejection_values=[];rejection_censored=[];rejections=[];tid_report={}
    for tid,events in by_tid.items():
        # Thread-local waits/admissions are analyzed in observed timestamp order.
        events.sort(key=lambda r:(r['ns'],r['seq']))
        pending=None;local_values=[];local_results=dict(timeout=0,signaled=0,other=0)
        for row in events:
            if row['event']==11:
                if pending is not None:raise ValueError(f'Overlapping IRQ waits on TID {tid}')
                pending=row
            elif row['event']==12:
                if pending is None:
                    wait_censored.append(dict(tid=tid,side='begin_missing',end_ns=row['ns'],result=row['b'],timeout_ms=row['a']))
                else:
                    if row['a']!=pending['a']:raise ValueError(f'IRQ wait timeout changed on TID {tid}')
                    delta=row['ns']-pending['ns'];wait_values.append(delta);local_values.append(delta)
                    category='signaled' if row['b']==0 else 'timeout' if row['b']==258 else 'other'
                    results[category]+=1;local_results[category]+=1;pending=None
        if pending:
            wait_censored.append(dict(tid=tid,side='end_missing',begin_ns=pending['ns'],timeout_ms=pending['a'],observed_to_window_end_ns=end-pending['ns']))
        next_admits={};local_rejections=[];local_levels=defaultdict(list)
        for row in reversed(events):
            if row['event']==15:next_admits[row['a'] & 0xffffffff]=row
            elif row['event'] in (13,14):
                level=row['a'] & 0xffffffff
                next_admit=next_admits.get(level)
                entry=dict(tid=tid,seq=row['seq'],ns=row['ns'],reason=EVENTS[row['event']],requested_level=level,a=row['a'],b=row['b'])
                if next_admit is None:
                    entry['next_admit_ns']=None;entry['elapsed_ns']=None;entry['observed_to_window_end_ns']=end-row['ns']
                    rejection_censored.append(entry)
                else:
                    delta=next_admit['ns']-row['ns'];entry['next_admit_ns']=next_admit['ns'];entry['elapsed_ns']=delta
                    rejection_values.append(delta);local_rejections.append(delta);local_levels[str(level)].append(delta);rejections.append(entry)
        tid_report[str(tid)]=dict(event_counts=dict(Counter(EVENTS[row['event']] for row in events)),
                                  wait_results=local_results,wait_durations_ns=distribution(local_values),
                                  rejection_to_next_admit_ns=distribution(local_rejections),
                                  rejection_to_next_admit_by_requested_level={level:distribution(values) for level,values in local_levels.items()})
    return (dict(results=results,durations_ns=distribution(wait_values),censored_waits=wait_censored,
                 scope='Same-TID WAIT_BEGIN/END pairs; elapsed includes observation overhead. Results classify only complete pairs: 0 signaled, 258 timeout, other raw results.'),
            dict(to_next_admit_ns=distribution(rejection_values),observed_rejections=rejections,censored_rejections=rejection_censored,
                 scope='Each rejection to the next actual IRQ_ADMIT on the same dispatcher TID AND requested level (low32 of a). Repeated rejections overlap. The next successful admission need not service the same pending IRQ or frame; IRQ level can deassert/change meanwhile. Releases/STATE events never imply admissibility; censored values are null, not zero.'),tid_report)


def d3d_summary(rows):
    calls={}
    for row in rows:
        if row['event'] not in (30,31):continue
        if not 0<row['id']<=0xffffffff:
            raise ValueError('Invalid original D3D virtual address')
        call=calls.setdefault(hex(row['id']),dict(entries=0,register_records=0,entry_tids={}))
        if row['event']==30:
            call['entries']+=1
            tid=str(row['tid']);call['entry_tids'][tid]=call['entry_tids'].get(tid,0)+1
        else:call['register_records']+=1
    return dict(by_raw_va=calls,
                scope='Raw original function entries, not draws, consumed commands, GPU work or presents. Wrappers and leaves can count the same operation. REG records are counted separately; no pairing, argument reconstruction or complete coverage is inferred.')

def batch_summary(rows):
    grouped=defaultdict(dict)
    for row in rows:
        event=row['event']
        if event not in (40,41,42):continue
        if not row['id'] or row['b']!=0:raise ValueError('Invalid batch record serial or reserved value')
        phases=grouped[row['id']]
        if event in phases:raise ValueError('Duplicate batch phase')
        phases[event]=row
    chains=[];durations=dict(record_to_submit=[],submit_to_retire=[]);complete=0
    for serial,phases in sorted(grouped.items()):
        seen=[phases[e] for e in (40,41,42) if e in phases]
        if len({r['tid'] for r in seen})!=1:raise ValueError('Batch worker TID changed')
        if any(a['ns']>b['ns'] for a,b in zip(seen,seen[1:])):raise ValueError('Batch causal timestamp inversion')
        chain=dict(record=serial,tid=seen[0]['tid'],missing_phases=[EVENTS[e] for e in (40,41,42) if e not in phases],
                   phases={EVENTS[e]:dict(ns=r['ns'],seq=r['seq'],draw_time=r['a']) for e,r in phases.items()})
        for key,(a,b) in dict(record_to_submit=(40,41),submit_to_retire=(41,42)).items():
            value=phases[b]['ns']-phases[a]['ns'] if a in phases and b in phases else None
            chain[key+'_ns']=value
            if value is not None:durations[key].append(value)
        if len(phases)==3:complete+=1
        chains.append(chain)
    overlap=[]
    for serial,phases in grouped.items():
        begin=phases.get(40)
        if begin is None:continue
        for prior,older in grouped.items():
            submit=older.get(41);retire=older.get(42)
            if prior>=serial or submit is None or retire is None or submit['tid']!=begin['tid']:continue
            if submit['ns']<=begin['ns']<retire['ns']:
                overlap.append(dict(record=serial,prior_record=prior,begin_ns=begin['ns'],prior_retired_ns=retire['ns']))
    return dict(complete_chains=complete,chains=chains,durations_ns={key:distribution(values) for key,values in durations.items()},
                begin_while_prior_submitted=overlap,
                scope='Main record lifecycle on its worker. BEGIN observes successful command-buffer opening; SUBMITTED follows queue submission; RETIRED follows real fence and validated resource cleanup. Submit-to-retire includes host scheduling and cleanup, never GPU execution time. Missing phases remain null. Overlap needs both prior endpoints and does not prove GPU hardware was still busy. Aux work, WSI and VBLANK are separate.')


def barrier_summary(rows):
    # Private payload v1 from prepare-xemu-live-barrier-trace.py. Availability
    # bits are essential: bootstrap/foreign contexts do not imply zero work.
    names=('NOP_NONZERO','SET_CONTEXT_DMA_REPORT','CLEAR_REPORT_VALUE','GET_REPORT',
           'ALLOCATION_LIFECYCLE','CPU_ACCESS_CONFLICT','CPU_STAMP_CONFLICT',
           'SOURCE_VERSION_CONFLICT','DMA_READ_CONFLICT','FIFO_DISPATCH_READ_CONFLICT',
           'FIFO_FETCH_READ_CONFLICT','RELEASE_QUEUE_CAPACITY','RELEASE_CURRENT_READ_CONFLICT',
           'MEMORY_BUDGET','HOST_DESTROY','DISPLAY','COMPUTE_PREPARE','STANDALONE_AUX',
           'VERTEX_BANK_DRAIN','OWNER_SLOT_DRAIN','CACHE_SLOT_DRAIN','REPORT_BANK_DRAIN',
           'RENDERER_DESTROY','RENDERER_FLUSH','WAIT_FOR_IDLE','WAIT_MMIO_CONSUMER')
    causes={i:name for i,name in enumerate(names,1)}
    causes.update({256+i:'LEGACY_'+name for i,name in enumerate((
        'VERTEX_BUFFER_DIRTY','SURFACE_CREATE','SURFACE_DOWN','NEED_BUFFER_SPACE',
        'FRAMEBUFFER_DIRTY','PRESENTING','FLIP_STALL','FLUSH','STALLED'))})
    def payload(row):
        a,b=row['a'],row['b'];cause=a&65535;flags=(a>>16)&65535
        pending=(a>>32)&65535;submitted=b>>32;completed=b&0xffffffff
        if cause not in causes or flags&~2047:raise ValueError('Invalid barrier cause/flags')
        if (flags&512 and not flags&256) or (row['event']==43 and flags&256):
            raise ValueError('Invalid barrier result flags')
        if (not flags&1 and b) or (flags&1 and completed>submitted):
            raise ValueError('Invalid barrier sequence availability/order')
        if not flags&32 and pending:raise ValueError('Invalid barrier RELEASE availability')
        return dict(ns=row['ns'],seq=row['seq'],cause=causes[cause],depth=a>>48,
            flags=flags,pending_releases=pending if flags&32 else None,
            submitted=submitted if flags&1 else None,completed=completed if flags&1 else None,
            result_success=bool(flags&512) if flags&256 else None)
    grouped=defaultdict(dict)
    for row in rows:
        if row['event'] not in (43,44):continue
        if not row['id'] or (row['event']==43 and row['id']!=row['seq']):
            raise ValueError('Invalid barrier reservation ID')
        phases=grouped[row['id']]
        if row['event'] in phases:raise ValueError('Duplicate barrier phase')
        phases[row['event']]=(row,payload(row))
    chains=[];by_cause={};intervals=defaultdict(list);complete=0
    for id_,phases in sorted(grouped.items()):
        observed=list(phases.values());row,p=observed[0]
        if any(r['tid']!=row['tid'] or q['cause']!=p['cause'] or q['depth']!=p['depth'] for r,q in observed):
            raise ValueError('Barrier identity/cause/depth changed')
        begin=phases.get(43);end=phases.get(44);duration=None
        if begin and end:
            if begin[0]['ns']>end[0]['ns'] or begin[0]['seq']>=end[0]['seq']:
                raise ValueError('Barrier causal timestamp/reservation inversion')
            duration=end[0]['ns']-begin[0]['ns'];complete+=1
            intervals[str(row['tid'])].append((begin[0]['ns'],end[0]['ns']))
        chain=dict(id=id_,tid=row['tid'],cause=p['cause'],depth=p['depth'],
            begin=begin[1] if begin else None,end=end[1] if end else None,inclusive_duration_ns=duration)
        chains.append(chain)
        cause=by_cause.setdefault(p['cause'],dict(observed_chains=0,complete_chains=0,durations=[]))
        cause['observed_chains']+=1
        if duration is not None:cause['complete_chains']+=1;cause['durations'].append(duration)
    # Validate complete intervals without fabricating parents for window cuts.
    # Depth alone cannot reject crossed END pairs; reservation order on the
    # same serialized producer supplies the nesting relation.
    per_tid=defaultdict(list)
    for chain in chains:
        if chain['inclusive_duration_ns'] is not None:per_tid[chain['tid']].append(chain)
    for observed in per_tid.values():
        stack=[]
        for chain in sorted(observed,key=lambda c:c['begin']['seq']):
            while stack and stack[-1]['end']['seq']<chain['begin']['seq']:stack.pop()
            if stack:
                parent=stack[-1]
                if chain['end']['seq']>parent['end']['seq']:
                    raise ValueError('Crossed barrier nesting')
                overflow=(chain['begin']['flags']|parent['begin']['flags'])&1024
                if chain['depth']<=parent['depth'] and not overflow:
                    raise ValueError('Invalid nested barrier depth')
            stack.append(chain)
    def union(values):
        if not values:return None
        ordered=sorted(values);begin,end=ordered[0];total=0
        for a,b in ordered[1:]:
            if a>end:total+=end-begin;begin,end=a,b
            else:end=max(end,b)
        return total+end-begin
    for cause in by_cause.values():cause['inclusive_duration_ns']=distribution(cause.pop('durations'))
    return dict(complete_chains=complete,censored_chains=len(chains)-complete,chains=chains,
        by_cause=by_cause,union_ns_by_tid={tid:union(values) for tid,values in intervals.items()},
        union_ns_across_tids=union([v for values in intervals.values() for v in values]),
        scope='Observed BOTH calls with original results and local snapshots. Caller and legacy durations are nested and must never be added. Unions cover only complete pairs; window-cut intervals remain unknown. Host elapsed time includes work, scheduling and observation overhead, not GPU execution time or a frame partition. END never proves GPU completion, presentation, VBLANK or success without an explicit known result.')


def analyze_bytes(data, game_log=None):
    header,rows,inversions=decode_records(data)
    waits,rejections,tids=irq_summary(rows,header['end_ns'])
    counts=Counter(row['event'] for row in rows)
    return dict(format='b3-live-causal-trace-v1',header=header,records=len(rows),
                reservation_timestamp_inversions=inversions,
                event_counts={name:counts[event] for event,name in EVENTS.items()},
                workers=worker_summary(rows),irq_waits=waits,irq_rejections=rejections,irq_by_tid=tids,
                d3d_entrypoints=d3d_summary(rows),
                main_batches=batch_summary(rows),
                barriers=barrier_summary(rows),
                completeness=read_stats(game_log,len(rows)),
                notes=['Sequence is reservation order, not a global timestamp order. Different-producer timestamp inversions are valid.',
                       'Timing distributions describe captured samples only; losses/window cuts can bias them. No durations are inferred for missing endpoints.',
                       'Worker tags are raw runtime addresses. PIE load base/symbol mapping is not inferred; no invented operation names.',
                       'IRQ_LEVEL/STATE, NOP, ISR, W1C and GET are observations. None proves GPU submission/completion, presentation or VBLANK.',
                       'Counts and timing sums may overlap across threads/requests and nested phases; they do not partition frame time.'])


class TraceTests(unittest.TestCase):
    def test_batch_phases_do_not_invent_completion_and_report_host_overlap(self):
        rows=[(110,9,40,1,3,0),(120,9,41,1,4,0),
              (130,9,40,2,4,0),(140,9,42,1,4,0),
              (150,9,41,2,5,0),(170,9,42,2,5,0),
              (180,9,41,3,5,0),(190,9,42,3,5,0),
              (200,9,40,4,5,0)]
        out=analyze_bytes(self.binary(rows))['main_batches']
        self.assertEqual(out['complete_chains'],2)
        self.assertEqual(out['durations_ns']['record_to_submit']['sum'],30)
        # The cut chain for serial3 has both submit/retire endpoints (10 ns).
        self.assertEqual(out['durations_ns']['submit_to_retire']['sum'],50)
        self.assertEqual(out['begin_while_prior_submitted'],[dict(record=2,prior_record=1,begin_ns=130,prior_retired_ns=140)])
        self.assertEqual(out['chains'][2]['missing_phases'],['batch_begin'])
        self.assertIsNone(out['chains'][2]['record_to_submit_ns'])
        self.assertIsNone(out['chains'][3]['submit_to_retire_ns'])

    def test_batch_invalid_phase_chains_are_rejected(self):
        cases=[[(110,9,40,0,3,0)],[(110,9,40,1,3,1)],
               [(110,9,40,1,3,0),(120,9,40,1,4,0)],
               [(110,9,40,1,3,0),(120,8,41,1,4,0)],
               [(120,9,40,1,3,0),(110,9,41,1,4,0)]]
        for rows in cases:
            with self.subTest(rows=rows),self.assertRaises(ValueError):analyze_bytes(self.binary(rows))

    def test_d3d_entries_are_raw_calls_not_draws_and_keep_missing_regs(self):
        rows=[(110,33,30,0x350000,0x100,5),
              (111,9,20,0,0,0),
              (112,33,31,0x350000,7,8),
              (120,34,30,0x350000,0x200,6),
              (121,33,31,0x3da90,1,2)]
        out=analyze_bytes(self.binary(rows))
        calls=out['d3d_entrypoints']['by_raw_va']
        self.assertEqual(calls['0x350000']['entries'],2)
        self.assertEqual(calls['0x350000']['register_records'],1)
        self.assertEqual(calls['0x350000']['entry_tids'],{'33':1,'34':1})
        self.assertEqual(calls['0x3da90']['entries'],0)
        self.assertEqual(calls['0x3da90']['register_records'],1)
        self.assertIn('not draws',out['d3d_entrypoints']['scope'])

    @staticmethod
    def binary(records=(), endian='<', begin=100, end=1000, capacity=100, **changes):
        header=[MAGIC,1,56,MARKER,begin,end,capacity,1]
        for key,value in changes.items():header[int(key[1:])]=value
        return struct.pack(endian+'8Q',*header)+b''.join(
            struct.pack(endian+'7Q',i+1,*r) for i,r in enumerate(records))

    def test_interleaved_worker_phases(self):
        rows=[(110,1,1,1,0xabc,0),(111,2,1,2,0xdef,0),
              (120,9,2,1,0xabc,0),(122,9,5,1,0xabc,0),
              (125,9,6,1,0xabc,0),(140,9,7,1,0xabc,0),
              (145,9,3,1,0xabc,3),(148,9,2,2,0xdef,0),
              (150,1,4,1,0xabc,3),(160,9,3,2,0xdef,0),(162,2,4,2,0xdef,0)]
        out=analyze_bytes(self.binary(rows))
        self.assertEqual(out['workers']['complete'],2)
        self.assertEqual(out['workers']['durations_ns']['queue']['sum'],47)
        self.assertEqual(out['workers']['durations_ns']['service']['sum'],37)
        self.assertEqual(out['workers']['durations_ns']['wake_return']['sum'],7)
        self.assertEqual(out['workers']['durations_ns']['gate_wait']['sum'],3)
        self.assertEqual(out['workers']['by_raw_tag']['0xabc']['count'],1)
        self.assertEqual(out['completeness']['status'],'unknown')

    def test_big_endian_and_reservation_timestamp_inversion(self):
        rows=[(150,1,20,0,0,0),(120,2,21,0,0,0)]
        out=analyze_bytes(self.binary(rows,endian='>'))
        self.assertEqual(out['header']['byte_order'],'big')
        self.assertEqual(out['reservation_timestamp_inversions'],1)
        self.assertEqual(out['event_counts']['nop'],1)

    def test_censored_chains_preserve_missing_durations(self):
        rows=[(110,9,2,42,0xaaa,0),(120,9,3,42,0xaaa,0),
              (125,1,1,3,0xbbb,0),(130,9,5,3,0xbbb,0),(135,9,6,3,0xbbb,0)]
        out=analyze_bytes(self.binary(rows))
        self.assertEqual(out['workers']['complete'],0)
        self.assertEqual(len(out['workers']['censored_chains']),2)
        self.assertEqual(out['workers']['durations_ns']['queue']['count'],0)
        self.assertIsNone(out['workers']['durations_ns']['queue']['sum'])
        self.assertEqual(out['workers']['durations_ns']['service']['sum'],10)
        self.assertEqual(out['workers']['anonymous_events'],0)

    def test_irq_wait_results_and_rejection_next_admit(self):
        rows=[(110,7,11,0,10,0),(120,7,12,0,10,258),
              (125,7,13,0,3,0),(127,8,14,0,1,2),
              (130,7,17,2,1,0),(140,7,15,0,3,0),
              (145,7,11,0,10,0),(150,7,12,0,10,0),
              (160,7,14,0,1,2),(165,8,11,0,10,0),(170,9,12,0,10,0)]
        out=analyze_bytes(self.binary(rows))
        self.assertEqual(out['irq_waits']['results'],{'timeout':1,'signaled':1,'other':0})
        self.assertEqual(out['irq_waits']['durations_ns']['sum'],15)
        self.assertEqual(len(out['irq_waits']['censored_waits']),2)
        self.assertEqual(out['irq_rejections']['to_next_admit_ns']['sum'],15)
        self.assertEqual(len(out['irq_rejections']['censored_rejections']),2)
        self.assertEqual(out['event_counts']['irq_state'],1)

    def test_irq_rejection_matches_requested_level_not_dpc(self):
        rows=[(110,7,13,0,3,0),(120,7,15,0,2,0),
              (125,7,14,0,(1<<32)|3,0),(130,7,10,0,0,0),
              (150,7,15,0,3,0),(160,7,13,0,2,0)]
        out=analyze_bytes(self.binary(rows))
        self.assertEqual(out['irq_rejections']['to_next_admit_ns']['sum'],65)
        self.assertEqual(out['irq_rejections']['to_next_admit_ns']['count'],2)
        self.assertEqual(len(out['irq_rejections']['censored_rejections']),1)
        self.assertEqual(out['irq_by_tid']['7']['rejection_to_next_admit_by_requested_level']['3']['sum'],65)
        self.assertEqual({row['requested_level'] for row in out['irq_rejections']['observed_rejections']},{3})

    def test_overflow_and_latest_log(self):
        data=self.binary([(110,1,20,0,0,0)],capacity=1)
        log='[XEMU-LIVE] causal_trace reserved=1 written=1 dropped=0 invalid=0 io_errors=0\n'
        log+='[XEMU-LIVE] causal_trace reserved=4 written=1 dropped=3 invalid=0 io_errors=0\n'
        out=analyze_bytes(data,log)
        self.assertEqual(out['completeness']['status'],'incomplete')
        self.assertEqual(out['completeness']['stats']['dropped'],3)
        out=analyze_bytes(data,log.splitlines()[0])
        self.assertEqual(out['completeness']['status'],'complete_captured_prefix')

    def test_log_pending_invalid_io_and_mismatch(self):
        data=self.binary([(110,1,20,0,0,0)])
        for tail in ('reserved=2 written=1 dropped=0 invalid=0 io_errors=0',
                     'reserved=1 written=1 dropped=0 invalid=1 io_errors=0',
                     'reserved=1 written=1 dropped=0 invalid=0 io_errors=1',
                     'reserved=1 written=0 dropped=0 invalid=0 io_errors=0'):
            with self.subTest(tail=tail):
                self.assertEqual(analyze_bytes(data,'[XEMU-LIVE] causal_trace '+tail)['completeness']['status'],'incomplete')
        for tail in ('reserved=-1','reserved=0.5','reserved=1 written=1 written=1',
                     'reserved=1 written=2 dropped=0 invalid=0 io_errors=0'):
            with self.subTest(tail=tail),self.assertRaises(ValueError):
                analyze_bytes(data,'[XEMU-LIVE] causal_trace '+tail)

    def test_log_nonmonotonic_and_missing_is_unknown(self):
        data=self.binary([(110,1,20,0,0,0)])
        with self.assertRaises(ValueError):
            analyze_bytes(data,'causal_trace reserved=3 written=1 dropped=2 invalid=0 io_errors=0\n'
                          'causal_trace reserved=1 written=1 dropped=0 invalid=0 io_errors=0')
        out=analyze_bytes(data,'causal_trace path=/tmp/a delay_ns=10 duration_ns=20 capacity=100')
        self.assertEqual(out['completeness']['status'],'unknown')
        self.assertIsNone(out['completeness']['requested_window_complete'])
        self.assertEqual(distribution([10,20,30,40])['p50'],25)
        self.assertEqual(distribution([10,20,30,40])['sum'],100)

    def test_truncated_and_bad_headers(self):
        data=self.binary([(110,1,20,0,0,0)])
        for bad in (b'',data[:63],data[:-1],data+b'\0',
                    self.binary(h0=0),self.binary(h1=2),self.binary(h2=55),
                    self.binary(h3=0),self.binary(begin=100,end=100),
                    self.binary(capacity=0),self.binary(capacity=1048577),self.binary(h7=0)):
            with self.subTest(size=len(bad)),self.assertRaises(ValueError):analyze_bytes(bad)

    def test_record_bounds_sequence_events(self):
        for rows,kwargs in (([(99,1,20,0,0,0)],{}),([(1000,1,20,0,0,0)],{}),
                            ([(110,0,20,0,0,0)],{}),([(110,1,999,0,0,0)],{}),
                            ([(110,1,20,0,0,0),(120,1,20,0,0,0)],{'capacity':1}),
                            ([(110,1,1,42,0,0)],{})):
            with self.subTest(rows=rows),self.assertRaises(ValueError):analyze_bytes(self.binary(rows,**kwargs))
        data=bytearray(self.binary([(110,1,20,0,0,0)]));struct.pack_into('<Q',data,64,2)
        with self.assertRaises(ValueError):analyze_bytes(data)

    def test_invalid_causal_chain(self):
        for rows in ([(130,1,1,1,5,0),(120,9,2,1,5,0)],
                     [(110,1,1,1,5,0),(120,9,2,1,5,0),(125,9,2,1,5,0)],
                     [(110,1,1,1,5,0),(120,9,2,1,6,0)],
                     [(110,7,11,0,10,0),(120,7,12,0,20,0)]):
            with self.subTest(rows=rows),self.assertRaises(ValueError):analyze_bytes(self.binary(rows))

    def test_empty_and_anonymous_events(self):
        out=analyze_bytes(self.binary())
        self.assertEqual(out['records'],0)
        self.assertIsNone(out['irq_waits']['durations_ns']['p50'])
        out=analyze_bytes(self.binary([(110,9,2,0,55,0)]))
        self.assertEqual(out['workers']['anonymous_events'],1)
        self.assertEqual(out['workers']['complete'],0)

    def test_wait_idle_barrier_and_unknown_cause(self):
        rows=[(110,7,43,1,25|(1<<16),0),
              (150,7,44,1,25|((1|256|512)<<16),0)]
        summary=barrier_summary(decode_records(self.binary(rows))[1])
        self.assertEqual(summary['complete_chains'],1)
        self.assertEqual(summary['chains'][0]['cause'],'WAIT_FOR_IDLE')
        self.assertEqual(summary['union_ns_across_tids'],40)
        bad=[(110,7,43,1,27|(1<<16),0)]
        with self.assertRaises(ValueError):barrier_summary(decode_records(self.binary(bad))[1])

    def test_wait_dependent_mmio_barrier(self):
        rows=[(110,7,43,1,26|(1<<16),0),
              (150,7,44,1,26|((1|256|512)<<16),0)]
        summary=barrier_summary(decode_records(self.binary(rows))[1])
        self.assertEqual(summary['chains'][0]['cause'],'WAIT_MMIO_CONSUMER')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace',nargs='?',type=Path)
    parser.add_argument('--game-log',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--self-test',action='store_true')
    args=parser.parse_args()
    if args.self_test:
        return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(TraceTests)).wasSuccessful() else 1
    if args.trace is None:parser.error('trace path required unless --self-test')
    try:
        data=args.trace.read_bytes()
        out=analyze_bytes(data,args.game_log.read_text(errors='replace') if args.game_log else None)
        out['trace_path']=str(args.trace.resolve());out['trace_sha256']=hashlib.sha256(data).hexdigest()
        if args.game_log:out['game_log_path']=str(args.game_log.resolve())
        rendered=json.dumps(out,indent=2,sort_keys=True)+'\n'
        if args.output:args.output.write_text(rendered)
        else:sys.stdout.write(rendered)
    except (OSError,ValueError) as error:
        print(f'trace analysis failed: {error}',file=sys.stderr);return 1
    return 0


if __name__=='__main__':raise SystemExit(main())
