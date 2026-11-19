"""Verify the exact reviewed native changes and preserve all other client bytes."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent.parent;client=Path('C:/DEV KAROS/CLIENTE - Copia')
b=(client/'AMo.exe').read_bytes();baseline=None
for p in (root/'build/client-command-prefix').glob('before-*/AMo.exe'):
    data=p.read_bytes()
    if hashlib.sha256(data).hexdigest()=='9423de697ce199fd7bac01f7b2e7783de68cdaa93a3b1d053d5e896d130bce19':baseline=data;break
assert baseline is not None and len(b)==len(baseline)
changed=[i for i,(a,c) in enumerate(zip(baseline,b)) if a!=c]
assert changed==[0x166c32,0x40643d],changed
assert b[0x40643c:0x40643f]==b'//\0'
assert b[0x166c31:0x166c36]==b'\x6a\x1e\x6a\x01\xe8'
guard=json.loads((client/'CLIENT_PROTECTED_FILES.json').read_text(encoding='utf-8-sig'));entry=next(r for r in guard['files'] if r['path'].lower()=='amo.exe')
assert entry['sha256']==hashlib.sha256(b).hexdigest()
print('PASS: exactly2 reviewed bytes changed; native // forwarding; /esc30s; all other executable bytes and native slash handlers preserved; protected executable hash updated')
