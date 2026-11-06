"""Verify the build ID and GNU debuglink CRC of a release ELF pair."""
import struct
import sys
import zlib
from pathlib import Path


def sections(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:5] != b'\x7fELF\x02' or header[5] not in (1, 2):
            raise ValueError('Expected a 64-bit ELF: ' + str(path))
        order = '<' if header[5] == 1 else '>'
        offset = struct.unpack_from(order + 'Q', header, 40)[0]
        size, count, names_index = struct.unpack_from(order + 'HHH', header, 58)
        if size < 64 or not count or names_index >= count:
            raise ValueError('Invalid ELF section table: ' + str(path))
        stream.seek(offset)
        table = stream.read(size * count)
        if len(table) != size * count:
            raise ValueError('Truncated ELF section table: ' + str(path))
        entries = [struct.unpack_from(order + 'IIQQQQIIQQ', table, i * size) for i in range(count)]
        names_entry = entries[names_index]
        stream.seek(names_entry[4])
        names = stream.read(names_entry[5])
        result = {}
        for entry in entries:
            name_end = names.find(b'\0', entry[0])
            if name_end < 0:
                raise ValueError('Invalid ELF section name: ' + str(path))
            name = names[entry[0]:name_end]
            if name not in (b'.gnu_debuglink', b'.note.gnu.build-id'):
                continue
            stream.seek(entry[4])
            data = stream.read(entry[5])
            if len(data) != entry[5]:
                raise ValueError('Truncated ELF metadata: ' + str(path))
            result[name] = data
        return order, result


def build_id(order, metadata):
    notes = metadata.get(b'.note.gnu.build-id', b'')
    at = 0
    while at + 12 <= len(notes):
        name_size, value_size, kind = struct.unpack_from(order + 'III', notes, at)
        at += 12
        name = notes[at:at + name_size]
        at += (name_size + 3) & ~3
        value = notes[at:at + value_size]
        at += (value_size + 3) & ~3
        if name.rstrip(b'\0') == b'GNU' and kind == 3 and len(value) == value_size and value:
            return value
    raise ValueError('Missing GNU build ID')


def check(binary, debug):
    order, metadata = sections(binary)
    debug_order, debug_metadata = sections(debug)
    if order != debug_order or build_id(order, metadata) != build_id(debug_order, debug_metadata):
        raise ValueError('ELF/debug build ID mismatch')
    link = metadata.get(b'.gnu_debuglink', b'')
    name_end = link.find(b'\0')
    crc_at = (name_end + 4) & ~3
    if name_end < 0 or len(link) != crc_at + 4 or link[:name_end] != debug.name.encode():
        raise ValueError('Missing or incorrect GNU debuglink filename')
    expected = struct.unpack_from(order + 'I', link, crc_at)[0]
    actual = 0
    with debug.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            actual = zlib.crc32(chunk, actual)
    if expected != actual & 0xffffffff:
        raise ValueError('GNU debuglink CRC mismatch')
    print('PASS ELF/debug build ID and CRC: ' + str(binary))


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3:
            raise ValueError('usage: check_elf_debug_companion.py <binary> <debug>')
        check(Path(sys.argv[1]).expanduser(), Path(sys.argv[2]).expanduser())
    except (OSError, ValueError, struct.error) as error:
        print('FAIL ELF/debug: ' + str(error), file=sys.stderr)
        sys.exit(1)
