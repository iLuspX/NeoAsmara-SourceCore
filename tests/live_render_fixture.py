"""Run the actual client with a synthetic account in a separate SQLite fixture.
No user password is read or entered; no original database is modified.
Stop by creating the reported fixture's STOP file.
"""
from pathlib import Path
import sys,json,sqlite3,socket,subprocess,threading,time,secrets,http.client,ctypes
from fixture_files import copy_verified as verified_copy
root=Path(__file__).resolve().parent.parent
site_package=Path('C:/DEV KAROS/Karos Server ((Test Functions))')
installed=Path(sys.argv[1]) if len(sys.argv)>1 else site_package
client=Path('C:/DEV KAROS/CLIENTE - Copia')
fixture=root/'build'/('live-render-fixture-'+str(time.time_ns()))
fixture.mkdir(parents=True)
with socket.socket() as probe:
    probe.bind(('127.0.0.1',10001))
for folder in ('Data','Config'):
    for p in (installed/folder).rglob('*'):
        if p.is_file():verified_copy(p,fixture/p.relative_to(installed))
(fixture/'Database').mkdir()
for p in (installed/'Database').glob('*.sqlite'):
    if p.name=='runtime.sqlite':continue
    with sqlite3.connect(p.resolve().as_uri()+'?mode=ro',uri=True) as src,sqlite3.connect(fixture/'Database'/p.name) as dst:src.backup(dst)
(fixture/'Config/server.json').write_text(json.dumps({'login_port':10001,'site_port':0}),encoding='utf-8')
(fixture/'GM_TEST_FIXTURE').write_text('isolated visual protocol fixture')
sys.path.insert(0,str(site_package/'SITE'))
from core_site import CppPortalServer
processes=[];server=None
try:
    for role in ['LobbyServer','WorldServer','LoginServer']:
        processes.append(subprocess.Popen([str(root/'build/native-debug'/f'{role}.exe'),'--root',str(fixture),'--headless','--start','--duration','1800'],creationflags=subprocess.CREATE_NO_WINDOW))
    deadline=time.monotonic()+15
    while True:
        try:
            with socket.create_connection(('127.0.0.1',10001),timeout=.3) as s:
                s.sendall(b'KAROS-LOCAL-STATUS\n')
                if s.recv(64)==b'KAROS-TEST-FUNCTIONS-AUTH/1\n':break
        except OSError:pass
        assert time.monotonic()<deadline,'Fixture gate timeout'
        time.sleep(.1)
    server=CppPortalServer(fixture,0);threading.Thread(target=server.serve_forever,daemon=True).start()
    def post(path,body,cookie=''):
        c=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=8)
        c.request('POST',path,json.dumps(body).encode(),{'Origin':server.origin,'Content-Type':'application/json','Cookie':cookie})
        r=c.getresponse();data=json.loads(r.read());headers=dict(r.getheaders());status=r.status;c.close();assert status<300,(path,status);return data,headers
    username='Render'+secrets.token_hex(4);password=secrets.token_urlsafe(20)
    post('/api/register',{'username':username,'password':password});_,headers=post('/api/login',{'username':username,'password':password});cookie=headers['Set-Cookie'].split(';')[0]
    with sqlite3.connect(fixture/'Database/karos_login.sqlite') as db:account=db.execute('SELECT id FROM login_accounts WHERE username=?',(username,)).fetchone()[0]
    with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:
        eid=max(62001,1+db.execute('SELECT COALESCE(MAX(eid),0) FROM character_locations').fetchone()[0])
        ch={'eid':eid,'name':'RenderProbe','cls':100,'level':1,'exp':0,'hp':100,'mp':100,'skill_points':0,'fletta':0,'fletta_energy':0,'moral_value':0,'god_mode':True}
        db.execute('INSERT INTO accounts(id,payload) VALUES(?,?)',(account,json.dumps({'chars':[ch],'next_eid':eid+1})))
        if '--fresh-spawn' not in sys.argv:
            db.execute('INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)',(eid,'berneo',7553.28,7402.36,612.52,0,time.time()))
    profile,_=post('/api/game-session',{},cookie);private=fixture/'launch-profile.json';private.write_text(json.dumps(profile),encoding='utf-8')
    subprocess.run([str(client/'KarosAsmaraLauncher.exe'),'--integration-test',str(private)],cwd=client,check=True)
    # The synthetic ticket is single use. The profile never appears in output.
    result={'fixture':str(fixture),'syntheticCharacter':'RenderProbe','pid':int((fixture/'native.pid').read_text()),'spawnMode':'new-character-without-checkpoint' if '--fresh-spawn' in sys.argv else 'diagnostic-spiderling-field','originalDatabasesModified':False}
    (root/'docs/live-render-fixture-current.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    while time.monotonic()<deadline+1700 and not (fixture/'STOP').exists() and all(p.poll() is None for p in processes):time.sleep(.5)
finally:
    if server:server.shutdown();server.server_close()
    k=ctypes.WinDLL('kernel32');k.OpenEventW.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_wchar_p];k.OpenEventW.restype=ctypes.c_void_p;k.SetEvent.argtypes=[ctypes.c_void_p];k.CloseHandle.argtypes=[ctypes.c_void_p]
    for p in processes:
        if p.poll() is None:
            h=k.OpenEventW(2,False,'Local\\RoshNativeStop_'+str(p.pid))
            if h:k.SetEvent(h);k.CloseHandle(h)
    for p in processes:
        if p.poll() is None:p.wait(timeout=20)
