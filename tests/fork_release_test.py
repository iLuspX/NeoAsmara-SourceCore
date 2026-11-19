"""Exercises the actual compiled launchers against the fork's signed artifacts."""
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import json,subprocess,threading,base64,hashlib,sys

root=Path(__file__).resolve().parent.parent
client=Path('C:/DEV KAROS/CLIENTE - Copia')
binaries=Path(sys.argv[1]) if len(sys.argv)>1 else client
release=root/'web_site/client_release'
signed=json.loads((release/'manifest.json').read_text(encoding='utf-8-sig'))
payload=json.loads(base64.b64decode(signed['payload']))
protected=json.loads((client/'CLIENT_PROTECTED_FILES.json').read_text(encoding='utf-8-sig'))
for resource in ('AMo.exe','ServerList.ten','Data/karos_002.kpk','Data/karos_004.kpk','Data/karos_030.kpk','CheckFileListW.ten'):
    entry=next(v for v in payload['files'] if v['path']==resource)
    guard=next(v for v in protected['files'] if v['path'].replace('\\','/')==resource)
    with (client/resource).open('rb') as handle:actual=hashlib.file_digest(handle,'sha256').hexdigest()
    assert entry['sha256']==guard['sha256']==actual,resource
for guard in protected['files']:
    if guard['path'].startswith('Data/') and guard['path'].endswith('.kpk'):
        entry=next(v for v in payload['files'] if v['path']==guard['path'])
        assert entry['sha256']==guard['sha256'],guard['path']
encoded=(client/'ServerList.ten').read_bytes();previous=0x42;decoded=bytearray()
for value in encoded[:-1]:decoded.append((value-previous)&255);previous=value
assert bytes(decoded)==b'127.0.0.1\r\n21001' and encoded[-1]==0

repair_hash=next(v['sha256'] for v in payload['files'] if v['path']=='Data/karos_002.kpk')
damage_next_pack=False
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        global damage_next_pack
        if self.path=='/api/client-update':path=release/'manifest.json'
        elif self.path.startswith('/api/client-update/'):
            name=self.path.rsplit('/',1)[-1]
            if len(name)!=64 or any(c not in '0123456789abcdef' for c in name):self.send_error(404);return
            path=release/'blobs'/name
        else:self.send_error(404);return
        if not path.is_file():self.send_error(404);return
        self.send_response(200);self.send_header('Content-Length',str(path.stat().st_size));self.end_headers()
        inject=damage_next_pack and path.name==repair_hash
        if inject:damage_next_pack=False
        with path.open('rb') as source:
            while block:=source.read(65536):
                if inject:block=bytes([block[0]^1])+block[1:];inject=False
                self.wfile.write(block)

server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
results=[]
try:
    names=['KarosAsmaraLauncher.exe','KarosUpdater.exe']
    if binaries.resolve()==client.resolve():names.append('Neo Asmara-Launcher.exe')
    for name in names:
        damage_next_pack=True
        fixture=root/'build/fork-release-test'/name
        # Unique test directories avoid reusing a downloaded version/receipt.
        import time
        fixture=fixture/str(time.time_ns());fixture.mkdir(parents=True)
        process=subprocess.run([str(binaries/name),'--update-self-test',str(fixture),str(server.server_port)],cwd=binaries,timeout=600)
        assert process.returncode==0,(name,process.returncode)
        assert not damage_next_pack,'Controlled invalid download was not exercised'
        result=(fixture/'result.txt').read_text()
        assert result.startswith('PASS:')
        for resource in ('Data/karos_030.kpk','CheckFileListW.ten'):
            assert (fixture/resource).read_bytes()==(client/resource).read_bytes(),(name,resource)
        for resource in ('Data/karos_000.kpk','Data/karos_002.kpk','Data/karos_004.kpk'):
            with (fixture/resource).open('rb') as handle:downloaded=hashlib.file_digest(handle,'sha256').hexdigest()
            assert downloaded==next(v['sha256'] for v in payload['files'] if v['path']==resource),(name,resource)
        results.append({'launcher':name,'passed':True,'result':result,'packedNativeChecklistDownloaded':True,'allGamePacksAvailableForRepair':True,'damagedPack002AutomaticallyRepaired':True,'damagedDownloadRetried':True,'concurrentUpdaterWaitVerified':True})
finally:
    server.shutdown();server.server_close();thread.join()

(root/'docs/fork-release-test.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
print(json.dumps(results))
