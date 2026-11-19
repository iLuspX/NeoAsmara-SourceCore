"""Byte-verified fixture copies; database files still use SQLite backup()."""
from pathlib import Path
import hashlib,os

def copy_verified(source,target):
    source,target=Path(source),Path(target)
    target.parent.mkdir(parents=True,exist_ok=True)
    expected=hashlib.sha256(source.read_bytes()).digest()
    for attempt in range(3):
        with source.open('rb') as origin,target.open('wb') as destination:
            while block:=origin.read(1024*1024):destination.write(block)
            destination.flush();os.fsync(destination.fileno())
        if hashlib.sha256(target.read_bytes()).digest()==expected:return target
    raise RuntimeError('Fixture copy changed bytes: '+source.name)

def copytree_verified(source,target):
    source,target=Path(source),Path(target)
    target.mkdir(parents=True,exist_ok=False)
    for path in source.rglob('*'):
        if path.is_symlink():raise RuntimeError('Fixture sources cannot contain links')
        if path.is_file():copy_verified(path,target/path.relative_to(source))
