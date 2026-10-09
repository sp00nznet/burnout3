#!/usr/bin/env python3
"""ENTRY58/EXIT59 wall scopes, nesting and same-caller queue exposure."""
import argparse
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

FOLDER=Path(__file__).resolve().parent
ROOT=FOLDER.parent
VAS={0x351090:'Swap',0x351770:'waiter',0x3518e0:'reserve'}

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value);return value

CADENCE=module('cadence',ROOT/'scripts/analyze-xemu-live-cadence.py')
CORE=module('critical',ROOT/'scripts/analyze-xemu-live-critical-path.py')
CADENCE.BASE.EVENTS.update({58:'xdk_scope_begin',59:'xdk_scope_end'})


def intersections(left,right):
    """Intersection of two sorted disjoint interval unions."""
    result=[];i=j=0
    while i<len(left) and j<len(right):
        a,b=left[i];c,d=right[j];lo,hi=max(a,c),min(b,d)
        if lo<hi:result.append((lo,hi))
        if b<=d:i+=1
        else:j+=1
    return result


def analyze(rows,begin,end):
    original=[r for r in rows if r['event']<50]
    critical=CORE.analyze_rows(original,begin,end)
    stacks=defaultdict(list);pending={};complete=[];censored=[];last_ns={}
    for row in sorted(rows,key=lambda r:r['seq']):
        if row['event'] not in (58,59):continue
        tid,id_,va,ns=row['tid'],row['id'],row['a'],row['ns']
        if va not in VAS or row['b']>0xffffffff:raise ValueError('Invalid XDK scope VA or guest32 scalar')
        if not begin<=ns<end or tid==0:raise ValueError('Invalid XDK scope timestamp/TID')
        if tid in last_ns and ns<last_ns[tid]:raise ValueError('XDK same-TID timestamp inversion')
        last_ns[tid]=ns
        if row['event']==58:
            if id_!=row['seq'] or id_==0 or id_ in pending:raise ValueError('Invalid/duplicate XDK BEGIN ID')
            scope=dict(id=id_,tid=tid,va=hex(va),name=VAS[va],begin_ns=ns,end_ns=None,
                       entry_esp=row['b'],exit_eax=None,parent_id=stacks[tid][-1] if stacks[tid] else None,
                       depth=len(stacks[tid]))
            pending[id_]=scope;stacks[tid].append(id_)
        elif id_==0:
            # begin returned0 outside the window; do not invent an earlier time.
            if stacks[tid]:raise ValueError('Censored outer EXIT while an observed child is still active')
            censored.append(dict(tid=tid,va=hex(va),missing='begin',end_ns=ns,duration_ns=None))
        else:
            if id_ not in pending:raise ValueError('Nonzero XDK EXIT without observed BEGIN')
            scope=pending[id_]
            if scope['tid']!=tid or scope['va']!=hex(va):raise ValueError('XDK EXIT changed TID/VA')
            if not stacks[tid] or stacks[tid][-1]!=id_:raise ValueError('Non-LIFO XDK EXIT')
            stacks[tid].pop();pending.pop(id_)
            scope.update(end_ns=ns,exit_eax=row['b'],inclusive_ns=ns-scope['begin_ns'])
            complete.append(scope)
    for scope in pending.values():
        censored.append(dict(scope,missing='end',duration_ns=None))
    grouped=defaultdict(list)
    for scope in complete:grouped[scope['tid']].append(scope)
    by_va={hex(va):dict(name=name,count=0,inclusive_sum_ns=0,inclusive_union_ns=0,
                      exclusive_ns=0,inclusive_queue_ns=0,inclusive_own_service_ns=0,
                      inclusive_wake_return_ns=0,exclusive_queue_ns=0,exclusive_own_service_ns=0,
                      exclusive_wake_return_ns=0) for va,name in VAS.items()}
    thread_reports={};global_union=[];queue_union=[];service_union=[];wake_union=[]
    for tid,scopes in grouped.items():
        intervals=[(s['begin_ns'],s['end_ns'],s['va'],s['depth']+1) for s in scopes]
        partition=CORE.partition_intervals(begin,end,intervals)
        union=CORE.Coverage([(s['begin_ns'],s['end_ns']) for s in scopes])
        global_union.extend(union.intervals)
        caller=critical['callers'].get(str(tid))
        phases={label:CORE.Coverage([(a,b) for a,b,l in caller['partition']['segments'] if l==label])
                if caller else CORE.Coverage([]) for label in ('queue','own_service','wake_return')}
        queue_union.extend(intersections(union.intervals,phases['queue'].intervals))
        service_union.extend(intersections(union.intervals,phases['own_service'].intervals))
        wake_union.extend(intersections(union.intervals,phases['wake_return'].intervals))
        thread_reports[str(tid)]=dict(scope_union_ns=union.between(begin,end),exclusive_totals_ns=partition['totals_ns'])
        for va in by_va:
            local=[s for s in scopes if s['va']==va]
            by_va[va]['count']+=len(local)
            by_va[va]['inclusive_sum_ns']+=sum(s['inclusive_ns'] for s in local)
            cov=CORE.Coverage([(s['begin_ns'],s['end_ns']) for s in local])
            by_va[va]['inclusive_union_ns']+=cov.between(begin,end)
            own=CORE.Coverage([(a,b) for a,b,l in partition['segments'] if l==va])
            by_va[va]['exclusive_ns']+=own.between(begin,end)
            for phase,phasecov in phases.items():
                by_va[va]['inclusive_'+phase+'_ns']+=sum(phasecov.between(a,b) for a,b in cov.intervals)
                by_va[va]['exclusive_'+phase+'_ns']+=sum(phasecov.between(a,b) for a,b in own.intervals)
        for scope in scopes:
            for label,cov in phases.items():scope[label+'_ns']=cov.between(scope['begin_ns'],scope['end_ns'])
    for va in by_va:
        by_va[va]['inclusive_durations']=CADENCE.distribution([s['inclusive_ns'] for s in complete if s['va']==va])
    return dict(window_begin_ns=begin,window_end_ns=end,complete_scope_count=len(complete),
                scope_union_ns=CORE.Coverage(global_union).between(begin,end),
                scope_sum_of_thread_unions_ns=sum(t['scope_union_ns'] for t in thread_reports.values()),
                scope_union_queue_ns=CORE.Coverage(queue_union).between(begin,end),
                scope_union_own_service_ns=CORE.Coverage(service_union).between(begin,end),
                scope_union_wake_return_ns=CORE.Coverage(wake_union).between(begin,end),
                by_va=by_va,threads=thread_reports,complete_scopes=sorted(complete,key=lambda s:s['id']),
                censored_scopes=censored,
                worker_summary={tid:{k:v for k,v in w.items() if k!='partition'} for tid,w in critical['workers'].items()},
                caller_partitions={tid:dict(request_count=c['request_count'],totals_ns=c['partition']['totals_ns'],overlap_ns=c['overlap_ns']) for tid,c in critical['callers'].items()},
                limits=['All durations are CLOCK_MONOTONIC elapsed wall including observer calls, blocking, preemption and nested children. No CPU awake/residency or removable savings claim.',
                        'Inclusive scopes overlap: Swap and reserve can contain waiter. Exclusive totals select deepest complete measured scope on the same TID; censored parents/children can leave attribution incomplete.',
                        'Queue/service/return intersects the same caller TID partition only, using complete endpoint pairs. Missing worker scopes are not silently filled.',
                        'Per-VA times sum thread domains; scope_union_ns is physical-wall union across TIDs. Those notions differ if multiple TIDs execute concurrently.',
                        'ID0 EXIT is censored; no duration, header-edge extension or synthetic BEGIN is invented.',
                        'Scope intersection with queue is observed blocking exposure, not independent savings. Original guest state and dependency correctness remain required.'])


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('trace',type=Path);parser.add_argument('--game-log',type=Path)
    args=parser.parse_args()
    try:
        data=args.trace.read_bytes();header,rows,inversions=CADENCE.BASE.decode_records(data)
        out=analyze(rows,header['begin_ns'],header['end_ns'])
        out['header']=header;out['source']=dict(path=str(args.trace.resolve()),sha256=hashlib.sha256(data).hexdigest(),bytes=len(data))
        out['completeness']=CADENCE.BASE.read_stats(args.game_log.read_text() if args.game_log else None,len(rows))
        out['projection']=dict(original_records=len(rows),selected_existing_events=sum(r['event']<50 for r in rows),original_seq_and_ids_preserved=True,reservation_contiguity_validated_before_projection=True)
        tuples=[(r['seq'],r['ns'],r['tid'],r['event'],r['id'],r['a'],r['b']) for r in rows]
        out['cadence']=CADENCE.analyze(tuples,header['begin_ns'],header['end_ns'])
        out['cadence']['completeness']=out['completeness']
        if out['completeness']['status']!='complete_captured_prefix':out['cadence']['observed_stall_including_censored_union_ns']=None
        out['analyzer_source']=dict(path=str(Path(__file__).resolve()),sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        print(json.dumps(out,indent=2));return 0
    except (ValueError,OSError) as exc:print('Invalid scope trace: '+str(exc),file=sys.stderr);return 2

if __name__=='__main__':sys.exit(main())
