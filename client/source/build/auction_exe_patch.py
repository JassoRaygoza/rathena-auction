"""Create a copy of Testing.exe with the native Auction window in a new PE section.

The original executable is never modified. The patch is refused unless the
input hash, the dispatcher entry and every client function/vtable slot used by
the embedded code match the static profile. The catalog build installs two packet hooks:

  1. New section .auctn at VA 0x04A15000 with the relocated code image built
     by client/auction/build/auction_section.vcxproj, with its exported
     auction_imports table filled with the Testing.exe addresses (IMPORTS_2026).
  2. Dispatcher table entry for packet 0x025F (0x00D15254) now points to
     auction_recv_025f instead of the default handler 0x00D14A7B.
  3. Packet 0x0252 is routed to the validated AUC2 response receiver.

Revert = keep using the original Testing.exe (or delete the generated copy).
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import capstone
import pefile

ORIGINAL_SHA256 = '95fbd61d250d3d7481b271d7d1928b2a03e8ebc9fae68fdeb68f2f74496e5a65'
SECTION_NAME = b'.auctn\0\0'
SECTION_VA = 0x04A15000
DISPATCH_SLOT = 0x00D14AA4 + 4 * (0x025F - 0x73)
DEFAULT_HANDLER = 0x00D14A7B
DISPATCH_CONTINUE = 0x00D08DAE
PACKET_BUFFER = 0x01720410
FRAME_VTABLE, FRAME_SLOTS = 0x01138A28, 53
# Client functions called by auction_window_2026.cpp with their first bytes.
FUNCTIONS = {
    0x00E6224F: 'operator new', 0x00E6227F: 'sized operator delete', 0x008A3700: 'UIFrameWnd constructor',
    0x008A4E90: 'UIFrameWnd destructor', 0x008BA1E0: 'UIFrameWnd left button down', 0x00A7B470: 'set size',
    0x00A8B9D0: 'add top-level window', 0x00AA9B40: 'focus', 0x00AA2C90: 'queue removal',
    0x00AF1620: 'resource manager', 0x00B01200: 'resolve UI path', 0x00AEE770: 'get bitmap',
    0x00A7BB60: 'draw bitmap', 0x00A80580: 'text width', 0x00A84360: 'text out',
}
FUNCTIONS.update({0x00A7BD60:'fill rectangle',0x00A7B710:'refresh',0x00C7D7D0:'network getter',
    0x00C7CE30:'send packet',0x0084FAE0:'edit constructor',0x00A7A080:'add child',
    0x00A81D40:'move child',0x004FA8F0:'assign edit string'})
FRAME_SLOT_EXPECTED = {0x10: 0x008AD150, 0x2C: 0x005CA0F0, 0x50: 0x006006C0, 0x64: 0x008BA1E0,
                       0x98: 0x00A81C30, 0xA4: 0x00A7B710, 0xB4: 0x005C8F60}
# Testing.exe values for the exported ClientImports table of auction_window.cpp,
# in the table's field order; every field is a u32.
IMPORTS_2026 = {
    'allocate': 0x00E6224F, 'release': 0x00E6227F, 'destroy_string': 0x004F98A0, 'assign_string': 0x004FA8F0,
    'construct': 0x008A3700, 'destruct': 0x008A4E90, 'base_on_event': 0x008BDC90, 'base_refresh': 0x00A7B710,
    'size_window': 0x00A7B470, 'move_window': 0x008AD150, 'add_child': 0x00A7A080, 'move_child': 0x00A81D40,
    'dirty': 0x00A81C30, 'set_window_id': 0x005C8F60, 'drag': 0x008BA1E0, 'drag_move': 0x008BA510,
    'drag_end': 0x008BA2C0, 'add_window': 0x00A8B9D0, 'remove_window': 0x00AA2C90, 'focus': 0x00AA9B40,
    'make_window': 0x00A978D0, 'capture_mouse': 0x00AA9B30, 'captured_window': 0x00A913F0,
    'release_mouse': 0x00AA66C0, 'edit_construct': 0x0084FAE0, 'fill': 0x00A7BD60,
    'resource_manager': 0x00AF1620, 'resolve_path': 0x00B01200, 'get_bitmap': 0x00AEE770,
    'draw_bitmap': 0x00A7BB60, 'measure': 0x00A80580, 'print_text': 0x00A84360,
    'item_descriptor': 0x006D5890, 'item_construct': 0x006D47E0, 'item_set_id': 0x006D9010,
    'network': 0x00C7D7D0, 'send_packet': 0x00C7CE30,
    'tick_import': 0x010C77F0, 'manager': 0x01457680, 'screen': 0x01389638, 'frame_vtable': FRAME_VTABLE,
    'measure_resume': 0x00A80585, 'text_resume': 0x00A84365, 'packet_buffer': PACKET_BUFFER,
    'dispatch_continue': DISPATCH_CONTINUE,
}
IMPORT_FIELDS = list(IMPORTS_2026)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def load_section(image_path):
    image = pefile.PE(str(image_path))
    if hasattr(image, 'DIRECTORY_ENTRY_IMPORT'):
        raise ValueError('Section image must not import anything')
    code = [s for s in image.sections if not s.Name.startswith(b'.reloc')]
    if len(code) != 1 or code[0].VirtualAddress != 0x1000:
        raise ValueError('Section image must have exactly one code section at RVA 0x1000')
    exports = {e.name.decode(): e.address for e in image.DIRECTORY_ENTRY_EXPORT.symbols}
    imports_size = image.get_dword_at_rva(exports['auction_imports_size'])
    if imports_size != 4 * len(IMPORTS_2026):
        raise ValueError(f'auction_imports_size {imports_size} does not match the {len(IMPORTS_2026)} known imports')
    image.relocate_image(SECTION_VA - 0x1000)
    section = image.sections[0]
    data = bytearray(section.get_data()[:section.SizeOfRawData].ljust(section.Misc_VirtualSize, b'\0'))
    # Fill the exported import table with this client's addresses.
    table_offset = exports['auction_imports'] - 0x1000
    if table_offset + imports_size > len(data) or any(data[table_offset:table_offset + imports_size]):
        raise ValueError('auction_imports is not an empty table inside the section')
    struct.pack_into(f'<{len(IMPORTS_2026)}I', data, table_offset, *IMPORTS_2026.values())
    data = bytes(data)
    field_va = lambda name: SECTION_VA + table_offset + 4 * IMPORT_FIELDS.index(name)
    entries = {op: SECTION_VA - 0x1000 + exports[name] for op,name in [(0x25f,'auction_recv_025f'),(0x252,'auction_recv_0252')]}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for entry in entries.values():
        stub = data[entry - SECTION_VA:entry - SECTION_VA + 21]
        insns = [(i.mnemonic, i.op_str) for i in md.disasm(stub, entry)]
        if len(insns) < 5 or insns[0] != ('push', f'dword ptr [{field_va("packet_buffer"):#x}]') \
                or insns[3] != ('push', f'dword ptr [{field_va("dispatch_continue"):#x}]') or insns[4][0] != 'ret':
            raise ValueError(f'Unexpected recv stub: {insns}')
    hooks={va:SECTION_VA-0x1000+exports[name] for va,name in [(0xA80580,'auction_measure_hook'),(0xA84360,'auction_text_hook')]}
    return data, entries, hooks, sha256(image_path.read_bytes())


def check_client(pe, raw, base):
    def read(va, size):
        return pe.get_data(va - base, size)
    if sha256(raw) != ORIGINAL_SHA256:
        raise ValueError('Unsupported Testing.exe hash; no patch applied')
    if pe.OPTIONAL_HEADER.DllCharacteristics & 0x40 or pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size:
        raise ValueError('Relocatable/ASLR image: fixed addresses would be unsafe')
    if any(s.Name == SECTION_NAME for s in pe.sections):
        raise ValueError('Auction section already present')
    last = pe.sections[-1]
    if base + last.VirtualAddress + align(last.Misc_VirtualSize, 0x1000) != SECTION_VA:
        raise ValueError('Unexpected end of image; section VA would overlap')
    if last.PointerToRawData + last.SizeOfRawData != len(raw):
        raise ValueError('Overlay data present; refusing to append a section')
    header_end = pe.sections[-1].get_file_offset() + 40
    if header_end + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
        raise ValueError('No room for another section header')
    if any(struct.unpack('<I', read(0x00D14AA4+4*(op-0x73), 4))[0] != DEFAULT_HANDLER for op in (0x25f,0x252)):
        raise ValueError('0x025F dispatcher entry is not the default handler')
    slots = struct.unpack(f'<{FRAME_SLOTS + 1}I', read(FRAME_VTABLE, 4 * (FRAME_SLOTS + 1)))
    for offset, target in FRAME_SLOT_EXPECTED.items():
        if slots[offset // 4] != target:
            raise ValueError(f'UIFrameWnd slot {offset:#x} changed')
    text = pe.sections[0]
    in_text = lambda va: text.VirtualAddress <= va - base < text.VirtualAddress + text.Misc_VirtualSize
    if not all(in_text(v) for v in slots[:FRAME_SLOTS]) or in_text(slots[FRAME_SLOTS]):
        raise ValueError('UIFrameWnd vtable size changed')
    return {f'{va:#010x}': {'name': name, 'first_16_bytes': read(va, 16).hex()} for va, name in FUNCTIONS.items()}


def build(exe, section_image, out):
    raw = bytearray(exe.read_bytes())
    pe = pefile.PE(data=bytes(raw))
    base = pe.OPTIONAL_HEADER.ImageBase
    functions = check_client(pe, bytes(raw), base)
    code, entries, hooks, image_hash = load_section(section_image)
    file_align = pe.OPTIONAL_HEADER.FileAlignment
    raw_offset = len(raw)
    raw_size = align(len(code), file_align)
    header = struct.pack('<8sIIIIIIHHI', SECTION_NAME, len(code), SECTION_VA - base, raw_size, raw_offset,
                         0, 0, 0, 0, 0xE0000060)
    header_offset = pe.sections[-1].get_file_offset() + 40
    raw[header_offset:header_offset + 40] = header
    file_header = pe.FILE_HEADER.get_file_offset()
    struct.pack_into('<H', raw, file_header + 2, pe.FILE_HEADER.NumberOfSections + 1)
    optional = pe.OPTIONAL_HEADER.get_file_offset()
    struct.pack_into('<I', raw, optional + 56, SECTION_VA - base + align(len(code), 0x1000))  # SizeOfImage
    # SizeOfCode grows by the new code section's raw size (the ClientPatcher engine's rule for code sections).
    struct.pack_into('<I', raw, optional + 4, pe.OPTIONAL_HEADER.SizeOfCode + raw_size)
    patches = []
    for va,target in hooks.items():
        offset=pe.get_offset_from_rva(va-base)
        before=bytes(raw[offset:offset+5])
        if before!=bytes.fromhex('558bec6aff'):raise ValueError('Font hook prologue changed')
        after=b'\xe9'+struct.pack('<i',target-va-5)
        raw[offset:offset+5]=after
        patches.append({'va':hex(va),'purpose':'Scoped Auction Arial / native edit font','before':before.hex(),'after':after.hex(),'target':hex(target)})
    for opcode,entry in entries.items():
        slot = 0x00D14AA4+4*(opcode-0x73)
        slot_offset = pe.get_offset_from_rva(slot-base)
        before = bytes(raw[slot_offset:slot_offset+4])
        struct.pack_into('<I', raw, slot_offset, entry)
        patches.append({'va':hex(slot),'purpose':f'packet {opcode:#06x} dispatcher entry',
                        'before':before.hex(),'after':struct.pack('<I',entry).hex(),'target':hex(entry)})
    raw += code.ljust(raw_size, b'\0')
    out.write_bytes(raw)
    check = pefile.PE(str(out))
    added = check.sections[-1]
    if added.Name != SECTION_NAME or check.get_data(added.VirtualAddress, len(code)) != code:
        raise ValueError('Written section does not match the image')
    for patch in patches:
        if check.get_data(int(patch['va'],16)-base,len(bytes.fromhex(patch['after'])))!=bytes.fromhex(patch['after']):raise ValueError('Written patch mismatch')
    if any(struct.unpack('<I', check.get_data(0x00D14AA4+4*(op-0x73)-base, 4))[0] != target for op,target in entries.items()):
        raise ValueError('Dispatcher entry not written')
    return {
        'method': 'appended_pe_section_and_dispatch_entry', 'original': str(exe.resolve()),
        'original_sha256': ORIGINAL_SHA256, 'output': str(out.resolve()), 'output_sha256': sha256(bytes(raw)),
        'section': {'name': '.auctn', 'va': hex(SECTION_VA), 'virtual_size': hex(len(code)),
                    'raw_offset': hex(raw_offset), 'raw_size': hex(raw_size), 'characteristics': '0xe0000060',
                    'sha256': sha256(code), 'image_sha256': image_hash,
                    'imports': {name: hex(value) for name, value in IMPORTS_2026.items()}},
        'patches': patches,
        'client_functions_checked': functions,
        'window_id': '0x2b80', 'packet': '0x025F type 0 opens, other types close',
        'limits': ['Live catalog via 0x0251/AUC2 0x0252; no built-in listings. Economic actions require server capabilities and explicit confirmation.',
                   'Statically derived ABI; the in-game behaviour must be observed before release.',
                   'Revert by using the untouched original executable.'],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('exe', type=Path)
    parser.add_argument('--section-image', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    if args.out.resolve() == args.exe.resolve():
        raise ValueError('Refusing to overwrite the original executable')
    if args.out.exists():
        raise ValueError(f'{args.out} already exists; remove it explicitly first')
    manifest = build(args.exe, args.section_image, args.out)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'output': manifest['output'], 'output_sha256': manifest['output_sha256'],
                      'section': manifest['section']['va'], 'entry': next(p['target'] for p in manifest['patches'] if p['purpose']=='packet 0x025f dispatcher entry')}))


if __name__ == '__main__':
    main()
