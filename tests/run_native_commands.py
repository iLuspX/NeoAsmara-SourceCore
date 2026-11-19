from pathlib import Path
import sqlite3,shutil,time,subprocess,json,hashlib,os,sys
root=Path(__file__).resolve().parent.parent
installed=Path(sys.argv[1]) if len(sys.argv)>1 else Path('C:/DEV KAROS/Karos Server ((Test Functions))')
fixture=root/'build'/('command-fixture-'+str(time.time_ns()))
fixture.mkdir(parents=True)
def verified_copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    expected=hashlib.sha256(source.read_bytes()).hexdigest()
    for attempt in range(3):
        with source.open('rb') as src,target.open('wb') as dst:
            while block:=src.read(1024*1024):dst.write(block)
            dst.flush();os.fsync(dst.fileno())
        if hashlib.sha256(target.read_bytes()).hexdigest()==expected:return
    raise RuntimeError('Fixture copy did not preserve bytes: '+source.name)
for folder in ('Data','Config'):
    for source in (installed/folder).rglob('*'):
        if source.is_file():verified_copy(source,fixture/source.relative_to(installed))
(fixture/'Database').mkdir()
database_source=installed/'Database'
if not database_source.is_dir():database_source=Path('C:/DEV KAROS/Karos Server ((Test Functions))/Database')
for p in database_source.glob('*.sqlite'):
    if p.name=='runtime.sqlite':continue
    with sqlite3.connect(p.resolve().as_uri()+'?mode=ro',uri=True) as source,sqlite3.connect(fixture/'Database'/p.name) as dest:source.backup(dest)
(fixture/'GM_TEST_FIXTURE').write_text('isolated command regression tests')
exe=root/'build/native-debug/KarosCommandTests.exe'
result=subprocess.run([str(exe),str(fixture)],capture_output=True,text=True,timeout=120)
report={'passed':result.returncode==0,'fixture':str(fixture),'stdout':result.stdout,'stderr':result.stderr,'exitCode':result.returncode,'liveDatabasesModified':False}
(root/'docs/native-command-verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report));raise SystemExit(result.returncode)
