"""Exercise compiled32bit launcher I/O against loopback fault-injected transfers."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,threading,sys
from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
ROOT=Path(__file__).resolve().parent.parent
fixture=Path(tempfile.mkdtemp(prefix='verified-io-',dir=ROOT/'build'));(fixture/'Data').mkdir()
direct=[]
for i,size in enumerate((19,1048576+137,0)):
 data=bytes((j*7+11)%256 for j in range(size));relative=f'Data/direct{i}.bin'
 (fixture/relative).write_bytes(data);direct.append(dict(path=relative,size=size,sha256=hashlib.sha256(data).hexdigest()))
data=bytes((j*13+3)%256 for j in range(262144+13));bad=bytes([data[0]^1])+data[1:];counts={}
(fixture/'fixture.json').write_text(json.dumps(dict(direct=direct,download=dict(size=len(data),sha256=hashlib.sha256(data).hexdigest()))),encoding='utf-8')
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  counts[self.path]=counts.get(self.path,0)+1
  body=bad if self.path.startswith('/bad/') or counts[self.path]==1 else data
  self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 args=['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(ROOT/'tests/verified_io_probe.ps1'),'-Fixture',str(fixture),'-Port',str(server.server_port)]
 if '--installed' in sys.argv:args+=['-Installed']
 result=subprocess.run(args,capture_output=True,text=True)
 print(result.stdout);print(result.stderr,file=sys.stderr)
 if result.returncode:raise RuntimeError('Compiled launcher verified I/O failed')
 assert all(counts.get('/retry/'+exe)==2 and counts.get('/bad/'+exe)==3 for exe in ('KarosUpdater.exe','KarosAsmaraLauncher.exe')),counts
 report=dict(passed=True,installed='--installed' in sys.argv,bothModes=True,alignedPartialAndEmptyReads=True,approvedDirectBytesOnly=True,networkCorruptionRetried=True,threeBadTransfersRejected=True,oldStagePreserved=True,progressMonotonic=True,gameStarted=False,requests=counts)
 (ROOT/'docs/verified-launcher-io-test.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report))
finally:server.shutdown();server.server_close()
