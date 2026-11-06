#!/usr/bin/env python3
"""Export and verify committed release inputs without changing a working tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(2**20), b''):
            h.update(block)
    return h.hexdigest()


def export(repo, ref, output):
    commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--verify', ref + '^{commit}'], text=True).strip()
    if len(commit) != 40 or any(c not in '0123456789abcdef' for c in commit):
        raise ValueError('Invalid commit')
    output = output.resolve()
    manifest_path = output / '.release-source.json'
    if not output.exists():
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=output.parent, suffix='.tar') as archive:
            subprocess.run(['git', '-C', str(repo), 'archive', '--format=tar', commit], stdout=archive, check=True)
            archive.flush()
            entries = {}
            with tarfile.open(archive.name) as source:
                for member in source.getmembers():
                    name = member.name
                    if Path(name).is_absolute() or '..' in Path(name).parts:
                        raise ValueError('Unsafe source path')
                    if member.isfile():
                        entries[name] = {'sha256': hashlib.sha256(source.extractfile(member).read()).hexdigest(),
                                         'executable': bool(member.mode & 0o111)}
                    elif member.issym():
                        target = Path(name).parent / member.linkname
                        if Path(member.linkname).is_absolute() or '..' in target.parts:
                            raise ValueError('Source symlink leaves export')
                        entries[name] = {'symlink': member.linkname}
                    elif not member.isdir():
                        raise ValueError('Unsupported archive entry')
                output.mkdir()
                source.extractall(output, filter='data')
            manifest_path.write_text(json.dumps({'commit': commit, 'files': entries}, indent=2) + '\n')
    manifest = json.loads(manifest_path.read_text())
    if manifest['commit'] != commit:
        raise ValueError('Export belongs to another commit')
    for name, record in manifest['files'].items():
        path = output / name
        if 'symlink' in record:
            if not path.is_symlink() or os.readlink(path) != record['symlink']:
                raise ValueError('Changed source symlink: ' + name)
        elif path.is_symlink() or not path.is_file() or digest(path) != record['sha256'] or bool(path.stat().st_mode & 0o111) != record['executable']:
            raise ValueError('Changed committed source: ' + name)
    # vendor_sqlite.sh independently verifies the four fixed third-party hashes.
    allowed_vendor = {'sqlite3.c', 'sqlite3.h', 'sqlite3ext.h', 'shell.c'}
    for path in output.rglob('*'):
        if not path.is_file() and not path.is_symlink():
            continue
        name = path.relative_to(output).as_posix()
        if name in manifest['files'] or path == manifest_path:
            continue
        if name.startswith('vexdb_sqlite/third_party/sqlite/') and path.name in allowed_vendor:
            continue
        raise ValueError('Unregistered file in release source: ' + name)
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--ref', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(export(args.repo, args.ref, args.out))


if __name__ == '__main__':
    main()
