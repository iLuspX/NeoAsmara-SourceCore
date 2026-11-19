import sys,hashlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'tools'))
from native_integrity import checked_resource
class Reader:
 def __init__(self,*blocks):self.blocks=list(blocks);self.calls=0
 def read(self,name):self.calls+=1;return self.blocks[min(self.calls-1,len(self.blocks)-1)]
good=b'reviewed immutable resource';bad=b'divergent resource';expected=hashlib.md5(good).hexdigest()
r=Reader(good);data,trace,passed=checked_resource(r,'item',expected);assert data==good and passed and trace is None and r.calls==1
r=Reader(bad,good,good);data,trace,passed=checked_resource(r,'item',expected);assert data==good and passed and trace['recoveredWithTwoIdenticalApprovedReads'] and r.calls==3
for sequence in [(bad,bad,bad),(bad,good,bad),(bad,bad,good)]:
 r=Reader(*sequence);data,trace,passed=checked_resource(r,'item',expected);assert not passed and not trace['recoveredWithTwoIdenticalApprovedReads'] and r.calls==3
print('PASS: exact approved reads; evidenced transient recovery; persistent or unstable mismatch rejected; no checksum authorization changed.')
