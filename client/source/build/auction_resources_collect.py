"""Collect the 15 local Auction BMPs into the legacy resource tree, with provenance.

Standard library only. Source files are read without modification. Existing
different destination files cause an error instead of being overwritten.
"""
import argparse
import hashlib
import html
import json
import struct
import zlib
from pathlib import Path
from urllib.parse import quote


NAMES = sorted(
    [f'auction_{tab}{state}.bmp' for tab in ('view', 'add', 'sell', 'buy')
     for state in ('', '_a')]
    + ['auction_msg.bmp', 'auction_menu.bmp', 'auction_menu_0.bmp', 'auction_menu_1.bmp']
    + [f'auction_bg_{i}.bmp' for i in range(3)]
)
UI_DIRECTORY = bytes.fromhex('c0afc0fac0cec5cdc6e4c0ccbdba').decode('cp949')


def read_grf_resources(path):
    """Read unencrypted v0x300 entries only; never modify the archive.

    Layout reference: Tokeiburu/GRFEditor, GRF/Core/{GrfHeader,FileTable,FileEntry}.cs.
    This is a narrow extractor, not a general GRF reader or encryption decoder.
    """
    stat = path.stat()
    with path.open('rb') as stream:
        header = stream.read(46)
        if len(header) != 46 or not header.startswith(b'Event Horizon\0'):
            raise ValueError('Expected Event Horizon header')
        version, = struct.unpack_from('<I', header, 42)
        if version != 0x300:
            raise ValueError(f'Unsupported GRF version: {version:#x}')
        table_offset, count = struct.unpack_from('<QI', header, 30)
        if table_offset + 58 > stat.st_size:
            raise ValueError('Table offset outside archive')
        stream.seek(46 + table_offset)
        reserved, compressed, expected = struct.unpack('<III', stream.read(12))
        if reserved != 0 or expected > 128 * 1024 * 1024 or compressed > 64 * 1024 * 1024:
            raise ValueError('Unsupported or excessive file table')
        packed = stream.read(compressed)
        decoder = zlib.decompressobj()
        table = decoder.decompress(packed, expected + 1)
        if len(table) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError('Invalid compressed file table')
        prefix = f'data\\texture\\{UI_DIRECTORY}\\basic_interface\\'.encode('cp949')
        wanted = {prefix + name.encode('ascii'): name for name in NAMES}
        found = {}
        pos = total = 0
        while pos < len(table):
            end = table.index(b'\0', pos)
            entry_name = table[pos:end]
            pos = end + 1
            csize, aligned, size, flags, offset = struct.unpack_from('<IIIBQ', table, pos)
            pos += 21
            total += 1
            if entry_name not in wanted:
                continue
            name = wanted[entry_name]
            if name in found or flags != 1 or csize > aligned or offset + 46 + aligned > stat.st_size:
                raise ValueError(f'Unsupported entry: {name}')
            if size > 16 * 1024 * 1024 or csize > 16 * 1024 * 1024:
                raise ValueError(f'Excessive resource size: {name}')
            stream.seek(46 + offset)
            entry_packed = stream.read(csize)
            decoder = zlib.decompressobj()
            data = decoder.decompress(entry_packed, size + 1)
            if len(data) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                raise ValueError(f'Invalid compressed resource: {name}')
            found[name] = (data, {'archive_entry': entry_name.decode('cp949'),
                                 'archive_offset': 46 + offset, 'compressed_size': csize,
                                 'compressed_sha256': hashlib.sha256(entry_packed).hexdigest()})
        if total != count or len(found) != len(NAMES):
            raise ValueError(f'Incomplete GRF inventory: {total}/{count}, resources {len(found)}/{len(NAMES)}')
    return found, {'path': str(path.resolve()), 'size': stat.st_size,
                   'mtime_ns': stat.st_mtime_ns, 'header_hex': header.hex(),
                   'version': hex(version), 'entry_count': count,
                   'compressed_table_sha256': hashlib.sha256(packed).hexdigest(),
                   'table_sha256': hashlib.sha256(table).hexdigest(),
                   'identity_note': 'Header/table/entry hashes; the entire GRF was not hashed.',
                   'format_reference': 'https://github.com/Tokeiburu/GRFEditor/blob/main/GRF/Core/FileTable.cs'}


def write_checked(path, data):
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError(f'Refusing to overwrite different file: {path}')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source_group = parser.add_mutually_exclusive_group(required=True)
    source_group.add_argument('--source', type=Path, help='Folder containing auction_*.bmp')
    source_group.add_argument('--grf', type=Path, help='Event Horizon v0x300 archive, unencrypted entries')
    parser.add_argument('--compare-source', type=Path, help='Verify that every extracted BMP also matches this loose folder')
    parser.add_argument('--source-note', help='Provenance/variant description included in the manifest and preview')
    parser.add_argument('--baseline-manifest', type=Path, help='Compare names, dimensions and hashes against an earlier collection')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    entries = []
    pending = []
    archive_entries, archive = read_grf_resources(args.grf) if args.grf else ({}, None)
    origin = args.grf or args.source
    origin_note = args.source_note or (
        'Extraídos del data.grf local de Cliente Limpio; no se certifica que sean la versión de 2012.'
        if args.grf else 'Copiados de archivos locales aportados para esta investigación; no se certifica su autoría ni versión histórica.')
    for name in NAMES:
        source = args.grf or (args.source / name)
        data, extra = archive_entries[name] if args.grf else (source.read_bytes(), {})
        if args.compare_source:
            comparison = args.compare_source / name
            if data != comparison.read_bytes():
                raise ValueError(f'Different comparison resource: {comparison}')
            extra['matching_loose_source'] = str(comparison.resolve())
        if len(data) < 54 or data[:2] != b'BM':
            raise ValueError(f'Not a BMP: {source}')
        size, = struct.unpack_from('<I', data, 2)
        dib_size, width, height, planes, bpp = struct.unpack_from('<IiiHH', data, 14)
        if size != len(data) or dib_size < 40 or width <= 0 or height == 0 or planes != 1:
            raise ValueError(f'Invalid BMP header: {source}')
        relative = Path('data') / 'texture' / UI_DIRECTORY / 'basic_interface' / name
        entries.append({'name': name, 'source': str(source.resolve()), **extra,
                        'destination': relative.as_posix(),
                        'grf_path_cp949_hex': str(relative).replace('/', '\\').encode('cp949').hex(),
                        'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                        'width': width, 'height': abs(height), 'bits_per_pixel': bpp})
        pending.append((args.out / relative, data))
    manifest = {'method': 'local_grf_zlib_extract' if args.grf else 'local_files_byte_identical_copy',
                'source_note': origin_note, 'archive': archive,
                'exe_sha256': 'd6e4a55f1f53e56799ea9de640a53138b985047a2c9ee07b1b0ae6941c7231c4',
                'file_count': len(entries), 'files': entries}
    if args.baseline_manifest:
        baseline_raw = args.baseline_manifest.read_bytes()
        baseline = {e['name']: e for e in json.loads(baseline_raw)['files']}
        comparisons = []
        if set(baseline) != set(NAMES):
            raise ValueError('Baseline file names do not match the expected 15 resources')
        for entry in entries:
            original = baseline[entry['name']]
            comparisons.append({'name': entry['name'],
                                'same_bytes': entry['sha256'] == original['sha256'],
                                'same_dimensions': (entry['width'], entry['height']) == (original['width'], original['height']),
                                'baseline_bits_per_pixel': original['bits_per_pixel'],
                                'bits_per_pixel': entry['bits_per_pixel']})
        manifest['comparison'] = {'baseline': str(args.baseline_manifest.resolve()),
                                  'baseline_manifest_sha256': hashlib.sha256(baseline_raw).hexdigest(),
                                  'files': comparisons}
    pending.append((args.out / 'manifest.json', (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8')))
    cards = []
    for entry in entries:
        cards.append(f'<figure><img src="{quote(entry["destination"])}" alt="{html.escape(entry["name"])}">'
                     f'<figcaption>{entry["name"]}<br>{entry["width"]} × {entry["height"]} · '
                     f'{entry["bits_per_pixel"]} bits</figcaption></figure>')
    preview = '''<!doctype html><html lang="es"><meta charset="utf-8"><title>Auction System Resources</title>
<style>body{font:16px system-ui;margin:32px;background:#eee;color:#222}main{display:flex;flex-wrap:wrap;gap:16px}
figure{margin:0;background:white;padding:16px;border:1px solid #bbb}img{max-width:100%;image-rendering:pixelated}
figcaption{font-size:14px;margin-top:12px}p{max-width:900px}</style>
<h1>Auction System Resources</h1><p>''' + html.escape(origin_note) + '''
Esta galería muestra recursos individuales; no es una captura de una ventana Auction funcionando.</p><main>''' + ''.join(cards) + '</main></html>\n'
    pending.append((args.out / 'Vista previa.html', preview.encode('utf-8')))
    readme = f'''# Auction System Resources

Recuperado el 2026-09-29 desde `{origin.resolve()}`.

Incluye 15 BMP de Auction: cuatro pestañas con estado normal/activo, tres fondos,
fondo de mensajes y tres imágenes de menú. {origin_note}

No se presenta esta colección como el diseño clásico original de Gravity.

## Ruta comprobada

`data/texture/{UI_DIRECTORY}/basic_interface/`

El EXE 2012 contiene el prefijo coreano en CP949, no `ZeroUI`. Se conservó el
contenido de los BMP. `manifest.json` incluye
origen, SHA-256, dimensiones y bytes CP949 de la ruta para importar en un GRF.
Evitar convertir los nombres internos CP949 a bytes UTF-8 al empaquetar el GRF.

Abrir `Vista previa.html` para revisar las imágenes. La carpeta `data` es un
árbol de recursos preparado para inspección/importación; no se instaló en ningún
cliente. La carga desde archivos sueltos depende de la configuración del EXE.

## Alcance

La colección contiene las imágenes con nombre `auction_*` encontradas localmente.
La ventana también usa controles compartidos, fuentes, textos de msgstringtable,
iconos de objetos y código del cliente, no incluidos aquí. No habilita Auction
en 2026 por sí sola. Los recursos no se probaron dentro del juego.

El manifiesto distingue copia local y extracción GRF. El extractor solo admite
las 15 entradas BMP sin cifrado del formato Event Horizon v0x300, con comprobación
de tamaños, límites, zlib y hashes. Referencia del formato:
[GRFEditor](https://github.com/Tokeiburu/GRFEditor/blob/main/GRF/Core/FileTable.cs).
No se modificó el GRF ni se extrajeron imágenes del EXE.
'''
    pending.append((args.out / 'LEEME.md', readme.encode('utf-8')))
    # Validate every existing destination before starting the copy.
    for path, data in pending:
        if path.exists() and path.read_bytes() != data:
            raise ValueError(f'Refusing to overwrite different file: {path}')
    for path, data in pending:
        write_checked(path, data)
        if path.read_bytes() != data:
            raise IOError(f'Copy verification failed: {path}')
    print(json.dumps({'output': str(args.out.resolve()), 'bmp_count': len(entries),
                      'bmp_bytes': sum(e['size'] for e in entries), 'verified': True}))


if __name__ == '__main__':
    main()
