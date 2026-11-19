from pathlib import Path
import json,sqlite3,time,subprocess,sys
from fixture_files import copy_verified
root=Path(__file__).resolve().parent.parent
source=Path(sys.argv[1])
installed=Path('C:/DEV KAROS/Karos Server ((Test Functions))')
fixture=root/'build'/('capture-replay-fixture-'+str(time.time_ns()));fixture.mkdir()
for folder in ('Data','Config'):
 for p in (source/folder).rglob('*'):
  if p.is_file():copy_verified(p,fixture/p.relative_to(source))
(fixture/'Database').mkdir()
for p in (installed/'Database').glob('*.sqlite'):
 if p.name=='runtime.sqlite':continue
 with sqlite3.connect(p.resolve().as_uri()+'?mode=ro',uri=True) as src,sqlite3.connect(fixture/'Database'/p.name) as dst:src.backup(dst)
(fixture/'GM_TEST_FIXTURE').write_text('capture replay; isolated databases')
result=subprocess.run([str(root/'build/native-debug/KarosCaptureReplayTests.exe'),str(fixture),str(root/'tests/fixtures/captured-mounts-20261026.json')],capture_output=True,text=True,timeout=120)
report={'passed':result.returncode==0,'fixture':str(fixture),'stdout':result.stdout,'stderr':result.stderr,'liveDatabasesModified':False}
(root/'docs/capture-replay-verification.json').write_text(json.dumps(report,indent=2));print(json.dumps(report));raise SystemExit(result.returncode)
