"""Read-only KPK1 v2 resource reader derived from supplied karos_aa.dll.
1001ff00 index layout, 10021f00 ChaCha20, 1001dd80 payload nonce.
Only explicit resource names are extracted. No client executable is run.
"""
from pathlib import Path
import struct,hashlib,json,subprocess,shutil
NODE=shutil.which('node')
if not NODE:
    portable=Path(__file__).resolve().parents[1]/'node/node.exe'
    if portable.is_file():NODE=str(portable)
CIPHER=Path(__file__).with_name('kpk_cipher.cjs')

def native_chacha(key,nonce,data,offset=0):
    return subprocess.run([NODE,str(CIPHER)],input=key+nonce+struct.pack('<Q',offset)+data,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout

def rot(x,n,bits):return ((x<<n)|(x>>(bits-n)))&((1<<bits)-1)
def chacha(key,nonce,data,offset=0):
    if NODE and CIPHER.exists() and len(data)>32768:return native_chacha(key,nonce,data,offset)
    out=bytearray();counter=offset//64;skip=offset%64
    for pos in range(0,len(data)+skip,64):
        s=list(struct.unpack('<4I',b'expand 32-byte k')+struct.unpack('<8I',key)+(counter,)+struct.unpack('<3I',nonce));x=s.copy()
        def qr(a,b,c,d):
            x[a]=(x[a]+x[b])&0xffffffff;x[d]=rot(x[d]^x[a],16,32)
            x[c]=(x[c]+x[d])&0xffffffff;x[b]=rot(x[b]^x[c],12,32)
            x[a]=(x[a]+x[b])&0xffffffff;x[d]=rot(x[d]^x[a],8,32)
            x[c]=(x[c]+x[d])&0xffffffff;x[b]=rot(x[b]^x[c],7,32)
        for _ in range(10):
            for q in ((0,4,8,12),(1,5,9,13),(2,6,10,14),(3,7,11,15),(0,5,10,15),(1,6,11,12),(2,7,8,13),(3,4,9,14)):qr(*q)
        block=struct.pack('<16I',*((a+b)&0xffffffff for a,b in zip(x,s)))
        start=max(0,pos-skip);part=data[start:min(len(data),pos+64-skip)]
        stream=block[skip if pos==0 else 0:]
        out.extend(a^b for a,b in zip(part,stream));counter+=1
    return bytes(out)

def siphash(key,data):
    mask=(1<<64)-1;k0,k1=struct.unpack('<QQ',key)
    v=[0x736f6d6570736575^k0,0x646f72616e646f6d^k1,0x6c7967656e657261^k0,0x7465646279746573^k1]
    def rnd():
        v[0]=(v[0]+v[1])&mask;v[1]=rot(v[1],13,64)^v[0];v[0]=rot(v[0],32,64)
        v[2]=(v[2]+v[3])&mask;v[3]=rot(v[3],16,64)^v[2]
        v[0]=(v[0]+v[3])&mask;v[3]=rot(v[3],21,64)^v[0]
        v[2]=(v[2]+v[1])&mask;v[1]=rot(v[1],17,64)^v[2];v[2]=rot(v[2],32,64)
    n=len(data)//8*8
    for i in range(0,n,8):
        m=int.from_bytes(data[i:i+8],'little');v[3]^=m;rnd();rnd();v[0]^=m
    m=(len(data)&255)<<56|int.from_bytes(data[n:],'little');v[3]^=m;rnd();rnd();v[0]^=m;v[2]^=255
    for _ in range(4):rnd()
    return v[0]^v[1]^v[2]^v[3]

class KPK:
    def __init__(self,client):
        self.client=Path(client)
        dll=(self.client/'karos_aa.dll').read_bytes()
        header=struct.unpack_from('<I',dll,0x3c)[0]
        if dll[header:header+4]!=b'PE\0\0':raise ValueError('Not a PE resource loader')
        optional=header+24
        if struct.unpack_from('<H',dll,optional)[0]!=0x10b:raise ValueError('Expected x86 resource loader')
        imagebase=struct.unpack_from('<I',dll,optional+28)[0]
        section_count=struct.unpack_from('<H',dll,header+6)[0]
        sections=optional+struct.unpack_from('<H',dll,header+20)[0]
        def at(va,n):
            rva=va-imagebase
            for i in range(section_count):
                virtual_size,start,raw_size,offset=struct.unpack_from('<IIII',dll,sections+40*i+8)
                if start<=rva and rva+n<=start+max(virtual_size,raw_size):
                    o=offset+rva-start;return dll[o:o+n]
            raise ValueError('Loader key outside PE sections')
        self.key=bytes(a^b for a,b in zip(at(0x1006f2c8,32),at(0x10078260,32)))
        data=(self.client/'Data/karos.idx').read_bytes()
        magic,version,self.packs,self.count=struct.unpack_from('<4sHHI',data)
        if magic!=b'KPK1' or version!=2 or self.count>100000:raise ValueError('Unsupported resource index')
        self.salt=data[12:28];self.entries={}
        self.index_bytes=data
        cache=Path(__file__).parent/'kpk_index_cache.json'
        digest=hashlib.sha256(data).hexdigest()
        if cache.is_file():
            saved=json.loads(cache.read_text())
            if saved.get('index_sha256')==digest:
                self.entries={int(h):tuple(v) for h,v in saved['entries'].items()};return
        decoded=None
        if NODE and CIPHER.exists():
            decoded=subprocess.run([NODE,str(CIPHER),'index'],input=self.key+data[32:32+28*self.count],stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout
        for i in range(self.count):
            raw=decoded[28*i:28*(i+1)] if decoded is not None else chacha(self.key,b'idx\0'+struct.pack('<I',i)+bytes(4),data[32+28*i:60+28*i])
            h,pack,flags,offset,size=struct.unpack('<QHHQQ',raw)
            if pack>=self.packs or size>256*1024*1024:raise ValueError('Index decryption/size check failed')
            self.entries[h]=(pack,flags,offset,size)
        cache.write_text(json.dumps({'index_sha256':digest,'entries':self.entries}))
    def entry(self,name):
        name=name.replace('\\','/').strip('/').lower().encode('utf-8')
        h=siphash(self.salt,name)
        if h not in self.entries:raise KeyError(name.decode())
        return self.entries[h]
    def read(self,name):
        pack,flags,offset,size=self.entry(name)
        p=self.client/f'Data/karos_{pack:03}.kpk'
        if offset+size>p.stat().st_size:raise ValueError('Resource outside package')
        with p.open('rb') as f:f.seek(offset);data=f.read(size)
        return chacha(self.key,struct.pack('<HHQ',pack,0,offset),data)

if __name__=='__main__':
    import sys
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'istoki_reverse/tools/pylibs'))
    k=KPK('C:/Users/Lucius/Desktop/Karos Genesis - Copia');print('Index entries:',len(k.entries))
    out=Path(__file__).parent/'recovered_resources';out.mkdir(exist_ok=True)
    for name in ['resunpak/interface/ie_lod/itemattr.lod_en','resunpak/interface/ie_lod/itemenchant.lod_en','resunpak/interface/ie_lod/cashshop.lod_en']:
        try:
            b=k.read(name);target=out/Path(name).name;target.write_bytes(b)
            print(name,len(b),hashlib.sha256(b).hexdigest())
        except KeyError:print('Name not in index:',name)
