"""Download the pinned official compiler; verify before extraction."""
from pathlib import Path
import hashlib,json,urllib.request,zipfile
root=Path(__file__).resolve().parent
record=json.loads((root/'native_toolchain.json').read_text())
name='llvm-mingw-'+record['compiler_release']+'-ucrt-x86_64'
destination=root/'toolchains'
compiler=destination/name/'bin/x86_64-w64-mingw32-clang++.exe'
if not compiler.is_file():
    archive=root/(name+'.zip')
    if not archive.is_file():
        print('Downloading pinned LLVM-MinGW compiler...',flush=True)
        request=urllib.request.Request(record['compiler_url'],headers={'User-Agent':'NeoAsmara-source-build'})
        with urllib.request.urlopen(request,timeout=60) as incoming,archive.open('xb') as output:
            while block:=incoming.read(1024*1024):output.write(block)
    with archive.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    if digest!=record['archive_sha256']:raise RuntimeError('Compiler archive checksum mismatch; extraction refused')
    destination.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        if sum(member.file_size for member in package.infolist())>4_000_000_000:raise RuntimeError('Unbounded compiler archive')
        for member in package.infolist():
            path=Path(member.filename)
            if '\\' in member.filename or ':' in member.filename or not (destination/path).resolve().is_relative_to(destination.resolve()):raise RuntimeError('Unsafe archive path')
        package.extractall(destination)
    archive.unlink()
if not compiler.is_file():raise RuntimeError('Compiler extraction is incomplete')
print(compiler)
