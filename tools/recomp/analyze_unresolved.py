#!/usr/bin/env python3
import json, bisect, os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..'))

UNRESOLVED_PATH = os.path.join(SCRIPT_DIR, 'output', 'unresolved_symbols.txt')
FUNCTIONS_PATH = os.path.join(REPO_ROOT, 'tools', 'disasm', 'output', 'functions.json')
OUTPUT_PATH = os.path.join(SCRIPT_DIR, 'output', 'missing_functions.json')

SECTIONS = [
    {'name': '.text', 'va': 0x00011000, 'size': 2863616},
    {'name': 'XMV', 'va': 0x002CC200, 'size': 163124},
    {'name': 'DSOUND', 'va': 0x002F3F40, 'size': 52668},
    {'name': 'WMADEC', 'va': 0x00300D00, 'size': 105828},
    {'name': 'XONLINE', 'va': 0x0031AA80, 'size': 124764},
    {'name': 'XNET', 'va': 0x003391E0, 'size': 78056},
    {'name': 'D3D', 'va': 0x0034C2E0, 'size': 83828},
    {'name': 'XGRPH', 'va': 0x00360A60, 'size': 8300},
    {'name': 'XPP', 'va': 0x00362AE0, 'size': 36052},
    {'name': '.rdata', 'va': 0x0036B7C0, 'size': 289684},
    {'name': '.data', 'va': 0x003B2360, 'size': 3904988},
    {'name': 'DOLBY', 'va': 0x0076B940, 'size': 29056},
    {'name': 'XON_RD', 'va': 0x00772AC0, 'size': 5416},
    {'name': '.data1', 'va': 0x00774000, 'size': 224},
]

TEXT_START = 0x00011000
TEXT_END = 0x00011000 + 2863616

def find_section(addr):
    for s in SECTIONS:
        if s['va'] <= addr < s['va'] + s['size']:
            return s['name']
    return None


def main():
    print('Loading function database...')
    with open(FUNCTIONS_PATH) as f:
        raw_funcs = json.load(f)

    functions = []
    for func in raw_funcs:
        start = int(func['start'], 16)
        end = int(func['end'], 16)
        name = func['name']
        functions.append((start, end, name))

    functions.sort(key=lambda x: x[0])
    func_starts = [f[0] for f in functions]
    print(f'  Loaded {len(functions)} functions')
    print(f'  Range: 0x{functions[0][0]:08X} - 0x{functions[-1][1]:08X}')

    print('Loading unresolved symbols...')
    with open(UNRESOLVED_PATH) as f:
        unresolved = []
        for line in f:
            line = line.strip()
            if not line: continue
            addr = int(line.replace('sub_', ''), 16)
            unresolved.append(addr)
    unresolved.sort()
    print(f'  Loaded {len(unresolved)} unresolved symbols')
    print(f'  Range: 0x{unresolved[0]:08X} - 0x{unresolved[-1]:08X}')

    results = {
        'mid_function': [],
        'continuation': [],
        'gap': [],
        'library_section': [],
        'data_section': [],
        'unknown': [],
    }
    missing_functions = []

    for addr in unresolved:
        section = find_section(addr)

        if section is None:
            results['unknown'].append(addr)
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'unknown', 'estimated_end': None})
            continue

        if section in ('.rdata', '.data', '.data1'):
            results['data_section'].append(addr)
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'data_section', 'section': section, 'estimated_end': None})
            continue

        if section != '.text':
            sec_info = next(s for s in SECTIONS if s['name'] == section)
            sec_end = sec_info['va'] + sec_info['size']
            results['library_section'].append((addr, section))
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'library_section', 'section': section, 'estimated_end': f'0x{sec_end:08X}'})
            continue

        idx = bisect.bisect_right(func_starts, addr) - 1

        if idx < 0:
            next_start = func_starts[0] if func_starts else TEXT_END
            results['gap'].append((addr, TEXT_START, next_start, next_start - addr))
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'gap', 'next_func_start': f'0x{next_start:08X}', 'estimated_end': f'0x{next_start:08X}', 'gap_size': next_start - addr})
            continue

        func_start, func_end, func_name = functions[idx]

        if func_start < addr < func_end:
            results['mid_function'].append((addr, func_start, func_name))
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'mid_function', 'parent_func': f'0x{func_start:08X}', 'parent_name': func_name, 'offset_into_func': addr - func_start, 'estimated_end': f'0x{func_end:08X}'})
            continue

        if addr == func_end:
            next_idx = idx + 1
            next_start = functions[next_idx][0] if next_idx < len(functions) else TEXT_END
            gap_to_next = next_start - addr
            results['continuation'].append((addr, func_start, func_name, gap_to_next))
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'continuation', 'parent_func': f'0x{func_start:08X}', 'parent_name': func_name, 'estimated_end': f'0x{next_start:08X}', 'gap_size': gap_to_next})
            continue

        if addr == func_start:
            print(f'  WARNING: Unresolved 0x{addr:08X} matches known function {func_name}')
            continue

        next_idx = idx + 1
        next_start = functions[next_idx][0] if next_idx < len(functions) else TEXT_END

        if func_end <= addr < next_start:
            gap_size = next_start - addr
            results['gap'].append((addr, func_end, next_start, gap_size))
            missing_functions.append({'address': f'0x{addr:08X}', 'type': 'gap', 'prev_func_end': f'0x{func_end:08X}', 'next_func_start': f'0x{next_start:08X}', 'estimated_end': f'0x{next_start:08X}', 'gap_size': gap_size})
            continue

        print(f'  WARNING: Could not classify 0x{addr:08X}')
        results['unknown'].append(addr)

    # Summary
    print()
    print('================================================================================')
    print('UNRESOLVED SYMBOL ANALYSIS SUMMARY')
    print('================================================================================')
    total = len(unresolved)
    print(f'\nTotal unresolved symbols: {total}')
    print()

    mid = results['mid_function']
    print(f'(a) MID-FUNCTION ENTRIES: {len(mid)} ({100*len(mid)/total:.1f}%)')
    print('    These addresses fall inside a known function body.')
    print('    Likely: tail-call targets, computed jumps, or function pointer offsets.')
    if mid:
        print('    Examples:')
        for addr, parent, name in mid[:10]:
            offset = addr - parent
            print(f'      0x{addr:08X}  (inside {name}, offset +0x{offset:X})')
    print()

    cont = results['continuation']
    print(f'(b) CONTINUATION PAST BOUNDARY: {len(cont)} ({100*len(cont)/total:.1f}%)')
    print('    Address falls exactly at a known function end.')
    print('    The function database underestimated the function size.')
    if cont:
        print('    Examples:')
        for addr, parent, name, gap in cont[:10]:
            print(f'      0x{addr:08X}  (after {name} ends, gap to next: {gap} bytes)')
        gaps = [g for _, _, _, g in cont]
        print(f'    Gap size stats: min={min(gaps)}, max={max(gaps)}, median={sorted(gaps)[len(gaps)//2]}, mean={sum(gaps)/len(gaps):.0f}')
    print()

    gap = results['gap']
    print(f'(c) GAP FUNCTIONS: {len(gap)} ({100*len(gap)/total:.1f}%)')
    print('    Addresses fall in gaps between known functions in .text.')
    print('    These are likely real functions the disassembler missed.')
    if gap and isinstance(gap[0], tuple):
        print('    Examples:')
        for item in gap[:10]:
            if len(item) == 4:
                a, pe, ns, gs = item
                print(f'      0x{a:08X}  (gap: 0x{pe:08X}-0x{ns:08X}, {gs} bytes)')
        gap_sizes = [item[3] for item in gap if isinstance(item, tuple) and len(item) == 4]
        if gap_sizes:
            print(f'    Gap size stats: min={min(gap_sizes)}, max={max(gap_sizes)}, median={sorted(gap_sizes)[len(gap_sizes)//2]}, mean={sum(gap_sizes)/len(gap_sizes):.0f}')
            print('    Gap size distribution:')
            for label, lo, hi in [('1-16 bytes',1,16),('17-64 bytes',17,64),('65-256 bytes',65,256),('257-1024 bytes',257,1024),('1025+ bytes',1025,999999999)]:
                count = sum(1 for s in gap_sizes if lo <= s <= hi)
                print(f'      {label:20s}: {count:5d}')
    print()

    lib = results['library_section']
    print(f'(d) LIBRARY SECTION FUNCTIONS: {len(lib)} ({100*len(lib)/total:.1f}%)')
    print('    Addresses fall in non-.text code sections (XDK libraries).')
    if lib:
        section_counts = {}
        for _, sec in lib:
            section_counts[sec] = section_counts.get(sec, 0) + 1
        print('    By section:')
        for sec, count in sorted(section_counts.items(), key=lambda x: -x[1]):
            sec_info = next(s for s in SECTIONS if s['name'] == sec)
            va = sec_info['va']
            sz = sec_info['size']
            print(f'      {sec:12s}: {count:4d} functions (section: 0x{va:08X}, {sz:,} bytes)')
        print('    Examples:')
        for a, sec in lib[:10]:
            print(f'      0x{a:08X}  ({sec})')
    print()

    data = results['data_section']
    print(f'(e) DATA SECTION REFERENCES: {len(data)} ({100*len(data)/total:.1f}%)')
    print('    Addresses fall in .rdata/.data - not code, likely misidentified.')
    if data:
        print('    Examples:')
        for a in data[:10]:
            sec = find_section(a)
            print(f'      0x{a:08X}  ({sec})')
    print()

    unk = results['unknown']
    if unk:
        print(f'(f) UNKNOWN (outside all sections): {len(unk)}')
        for a in unk[:5]:
            print(f'      0x{a:08X}')
    print()

    # Actionable summary
    print('================================================================================')
    print('ACTIONABLE SUMMARY')
    print('================================================================================')
    text_actionable = len(gap) + len(cont)
    print(f'\n  Symbols in .text that can extend function DB: {text_actionable} (gap: {len(gap)}, continuation: {len(cont)})')
    print(f'  Symbols in library sections (need lib disassembly):  {len(lib)}')
    print(f'  Mid-function entries (need parent func extension):   {len(mid)}')
    print(f'  Data section refs (need investigation):              {len(data)}')
    print(f'  Unknown:                                             {len(unk)}')
    print()

    if gap and isinstance(gap[0], tuple):
        unique_gaps = set()
        for item in gap:
            if len(item) == 4:
                _, pe, ns, _ = item
                unique_gaps.add((pe, ns))
        print(f'  Unique inter-function gaps with unresolved symbols: {len(unique_gaps)}')
        total_gap_bytes = sum(ns - pe for pe, ns in unique_gaps)
        print(f'  Total bytes in those gaps: {total_gap_bytes:,} ({total_gap_bytes/1024:.1f} KB)')

    if cont:
        unique_parents = set(parent for _, parent, _, _ in cont)
        print(f'  Functions needing boundary extension: {len(unique_parents)}')
        total_cont_bytes = sum(g for _, _, _, g in cont)
        print(f'  Total continuation bytes: {total_cont_bytes:,} ({total_cont_bytes/1024:.1f} KB)')

    if lib:
        lib_sections_used = set(sec for _, sec in lib)
        total_lib_bytes = sum(next(s for s in SECTIONS if s['name'] == sec)['size'] for sec in lib_sections_used)
        print(f'  Total library section bytes (all used sections): {total_lib_bytes:,} ({total_lib_bytes/1024:.1f} KB)')
    print()

    print(f'\nWriting {len(missing_functions)} entries to {OUTPUT_PATH}...')
    with open(OUTPUT_PATH, 'w') as f:
        json.dump(missing_functions, f, indent=2)
    print('Done.')

    quick_add_path = os.path.join(SCRIPT_DIR, 'output', 'addable_functions.json')
    addable = []
    for entry in missing_functions:
        if entry['type'] in ('gap', 'continuation', 'library_section'):
            addable.append({
                'address': entry['address'],
                'type': entry['type'],
                'estimated_end': entry.get('estimated_end'),
                'gap_size': entry.get('gap_size'),
                'section': entry.get('section', '.text'),
            })
    print(f'Writing {len(addable)} addable function candidates to {quick_add_path}...')
    with open(quick_add_path, 'w') as f:
        json.dump(addable, f, indent=2)
    print('Done.')


if __name__ == '__main__':
    main()
