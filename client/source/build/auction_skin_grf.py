"""Package only the generated X2 skin into an unencrypted Event Horizon v0x300 GRF.

Format follows the existing auction_resources_collect.py v0x300 reader.
Round-trip every member before reporting success. No original GRF is modified.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path
from auction_resources_collect import NAMES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('resources', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--buttons-manifest', type=Path, required=True)
    args = parser.parse_args()
    files = sorted((args.resources/'data').rglob('*.bmp'))
    button_manifest=json.loads(args.buttons_manifest.read_text(encoding='utf-8'))
    buttons={f['name']:f for f in button_manifest['files']}
    if {f.name for f in files} != set(NAMES)|set(buttons):
        raise ValueError('Incomplete original skin or bitmap button states')
    payload, table, records = bytearray(), bytearray(), []
    for file in files:
        name = str(file.relative_to(args.resources)).replace('/', '\\').encode('cp949')
        if b'\\auction_x2\\' not in name:
            raise ValueError('Package must not override original skin names')
        raw = file.read_bytes()
        if file.name in buttons:
            expected=buttons[file.name]
            if hashlib.sha256(raw).hexdigest()!=expected['sha256'] or struct.unpack_from('<ii',raw,18)!=(expected['width'],expected['height']):raise ValueError('Button resource mismatch')
        packed = zlib.compress(raw)
        table += name+b'\0'+struct.pack('<IIIBQ', len(packed), len(packed), len(raw), 1, len(payload))
        records.append({'path': name.decode('cp949'), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
        payload += packed
    compressed_table = zlib.compress(table)
    header = bytearray(46)
    header[:14] = b'Event Horizon\0'
    struct.pack_into('<QII', header, 30, len(payload), len(files), 0x300)
    archive = header+payload+struct.pack('<III', 0, len(compressed_table), len(table))+compressed_table
    pos = 0
    checked_table = zlib.decompress(archive[46+len(payload)+12:])
    for record in records:
        end = checked_table.index(0, pos)
        name = checked_table[pos:end].decode('cp949')
        csize, aligned, size, flags, offset = struct.unpack_from('<IIIBQ', checked_table, end+1)
        pos = end+22
        data = zlib.decompress(archive[46+offset:46+offset+csize])
        if name != record['path'] or flags != 1 or aligned != csize or size != len(data) or hashlib.sha256(data).hexdigest() != record['sha256']:
            raise ValueError('GRF round-trip mismatch')
    if pos != len(checked_table):
        raise ValueError('Unexpected trailing table data')
    with args.out.open('xb') as stream:
        stream.write(archive)
    args.out.with_suffix('.json').write_text(json.dumps({'version': '0x300', 'file_count': len(files), 'sha256': hashlib.sha256(archive).hexdigest(),
        'all_members_round_trip_verified': True, 'files': records}, indent=2)+'\n', encoding='utf-8')
    print(f'GRF: {len(files)}/{len(files)} resources verified.')


if __name__ == '__main__':
    main()
