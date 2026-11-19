from pathlib import Path
import json,sqlite3,sys,tempfile
from contextlib import closing
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'web_site'))
from title_public import appearance
catalog=json.loads((ROOT/'web_site/title_styles.json').read_text())
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder)/'test.sqlite'
 with closing(sqlite3.connect(p)) as db,db:
  db.executescript('CREATE TABLE accounts(id TEXT PRIMARY KEY,payload TEXT);CREATE TABLE character_titles(account_id TEXT,character_eid INTEGER,title_id INTEGER);CREATE TABLE character_title_selection(character_eid INTEGER,title_id INTEGER);')
  db.execute('INSERT INTO accounts VALUES(?,?)',('owner',json.dumps({'chars':[{'eid':10,'name':'Hero'}]})))
  db.execute('INSERT INTO character_title_selection VALUES(10,1001)')
 assert appearance(p,'Hero',catalog)['title'] is None
 with closing(sqlite3.connect(p)) as db,db:db.execute('INSERT INTO character_titles VALUES(?,?,?)',('other',10,1001))
 assert appearance(p,'Hero',catalog)['title'] is None
 with closing(sqlite3.connect(p)) as db,db:db.execute("UPDATE character_titles SET account_id='owner'")
 result=appearance(p,'[ADMIN] Hero',catalog)
 assert result=={'name':'Hero','title':{'id':1001,'label':'Player','theme':0}}
 with closing(sqlite3.connect(p)) as db,db:
  db.executescript('CREATE TABLE native_public_character_names(character_eid INTEGER PRIMARY KEY,account_id TEXT,name TEXT);CREATE INDEX native_name_lookup ON native_public_character_names(name);')
  db.execute('INSERT INTO native_public_character_names VALUES(10,?,?)',('owner','Hero'))
 assert appearance(p,'Hero',catalog)==result
 with closing(sqlite3.connect(p)) as db,db:db.execute("UPDATE character_titles SET account_id='other'")
 assert appearance(p,'Hero',catalog)['title'] is None
 for name in ('','x'*36,'Hero\n'):
  try:appearance(p,name,catalog)
  except ValueError:pass
  else:raise AssertionError('Malformed name accepted')
 assert appearance(p,'Missing',catalog)['title'] is None
print('PASS: read-only title ownership, indexed and legacy paths, no account disclosure, revocation and input validation.')
