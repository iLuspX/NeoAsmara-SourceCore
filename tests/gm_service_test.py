"""Real native GM API tests using copied databases; never grants real staff/items."""
from pathlib import Path
import ctypes, hashlib, http.client, json, secrets, shutil, socket, sqlite3, subprocess, sys, time
from fixture_files import copytree_verified,copy_verified

package=Path(sys.argv[1]); fixture=Path(sys.argv[2]); binary=Path(sys.argv[3])
assert not fixture.exists()
(fixture/'Database').mkdir(parents=True);(fixture/'Config/Keys').mkdir(parents=True)
(fixture/'GM_TEST_FIXTURE').write_text('Isolated copied databases. No production writes.\n')
(fixture/'Config/server.json').write_text(json.dumps({'gm_local_account_login_only':False}))
copytree_verified(package/'Data',fixture/'Data')
copy_verified(package/'Config/Keys/content.key.dpapi',fixture/'Config/Keys/content.key.dpapi')
# Bind only the synthetic fixture owner to exercise the real owner policy.
data_tool=next(p for p in (package/'Bin/KarosDataTool.exe',binary.parent/'KarosDataTool.exe') if p.is_file())
owner_source=fixture/'owner-test-source';(owner_source/'data').mkdir(parents=True)
(owner_source/'data/owner_authority.private.json').write_text(json.dumps({'staff_titles_owner_bound':True,'account_id':'gmfixture0'}))
owner_stage=fixture/'owner-test-stage';(owner_stage/'Config/Keys').mkdir(parents=True)
copy_verified(fixture/'Config/Keys/content.key.dpapi',owner_stage/'Config/Keys/content.key.dpapi')
subprocess.run([str(data_tool),'pack',str(owner_source),str(owner_stage)],check=True,capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
copy_verified(owner_stage/'Config/Keys/owner_authority.private.json.dat',fixture/'Config/Keys/owner_authority.private.json.dat')
for source in (package/'Database').glob('*.sqlite'):
 if source.name=='runtime.sqlite':continue
 with sqlite3.connect(source.as_uri()+'?mode=ro',uri=True) as origin, sqlite3.connect(fixture/'Database'/source.name) as dest:origin.backup(dest)
password=secrets.token_urlsafe(20);staff_password=secrets.token_urlsafe(20);salt=secrets.token_bytes(32)
digest=hashlib.pbkdf2_hmac('sha256',password.encode(),salt,600000)
staff_digest=hashlib.pbkdf2_hmac('sha256',staff_password.encode(),salt,600000)
with sqlite3.connect(fixture/'Database/karos_login.sqlite') as db:
 for i,role in enumerate(('Admin','GM','Mod')):
  db.execute('INSERT INTO login_accounts VALUES(?,?,?,?,?,?,?,?)',(f'gmfixture{i}',f'GMFixture{i}',f'gmfixture{i}',salt,digest,600000,time.time(),900000+i))
with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:
 for table in ('gm_actions','gm_mail','gm_spawns'):
  if db.execute('SELECT name FROM sqlite_master WHERE type=? AND name=?',('table',table)).fetchone():db.execute('DELETE FROM '+table)
 for i,role in enumerate(('Admin','GM','Mod')):
  db.execute('INSERT INTO accounts(id,payload) VALUES(?,?)',(f'gmfixture{i}',json.dumps({'chars':[{'eid':50001+i,'name':f'Fixture{role}','cls':100,'level':1,'pos':[9020,6590,627],'map':'berneo'}]})))
with sqlite3.connect(fixture/'Database/karos_roles.sqlite') as db:
 for i,role in enumerate(('Admin','GM','Mod')):
  db.execute('INSERT INTO staff_roles(account_id,character_eid,character_name,role,auth_generation) VALUES(?,?,?,?,?)',(f'gmfixture{i}',50001+i,f'Fixture{role}',role,f'fixture-generation-{i}'))
with sqlite3.connect(fixture/'Database/staff_second_login.sqlite') as db:
 for i in range(3):db.execute('INSERT INTO staff_credentials VALUES(?,?,?,?,?)',(f'gmfixture{i}',50001+i,f'fixture-generation-{i}',salt,staff_digest))
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
process=subprocess.Popen([str(binary),'--root',str(fixture),'--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NO_WINDOW)
def api(path,data=None,token='',deny=False,origin=False):
 connection=http.client.HTTPConnection('127.0.0.1',port,timeout=15)
 headers={'Content-Type':'application/json'}
 if token:headers['Authorization']='Bearer '+token
 if origin:headers['Origin']='https://example.invalid'
 try:
  connection.request('GET' if path=='/health' else 'POST',path,None if data is None else json.dumps(data),headers)
  response=connection.getresponse();payload=json.loads(response.read())
  if deny:assert response.status!=200,payload
  else:assert response.status==200,payload
  return payload
 finally:connection.close()
try:
 for _ in range(100):
  if process.poll() is not None:raise AssertionError(process.stderr.read().decode())
  try:
   if api('/health')['service']=='rosh-gm-native':break
  except OSError:time.sleep(.1)
 else:raise AssertionError('GM backend did not start')
 api('/players',{},deny=True)
 api('/login',dict(username='GMFixture2',password=password,character='FixtureMod',staff_password=staff_password),deny=True)
 api('/login',dict(username='GMFixture0',password=password,character='FixtureAdmin',staff_password='NotTheStaffPassword'),deny=True)
 admin=api('/login',dict(username='GMFixture0',password=password,character='FixtureAdmin',staff_password=staff_password))['token']
 gm=api('/login',dict(username='GMFixture1',password=password,character='FixtureGM',staff_password=staff_password))['token']
 with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:
  prior_title_audits=db.execute('select count(*) from title_audit').fetchone()[0]
 title_request=dict(eid=50002,title_id=2003,operation='grant',request_id=secrets.token_hex(16),reason='Fixture title management')
 assert api('/titles',{'eid':50002},gm)['may_change'] is False
 api('/titles/change',title_request,gm,deny=True)
 granted=api('/titles/change',title_request,admin)
 assert granted['may_change'] and next(t for t in granted['rows'] if t['id']==2003)['owned']
 assert api('/titles/change',title_request,admin)==granted
 with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:
  assert db.execute('select count(*) from title_audit').fetchone()[0]==prior_title_audits+1
  db.execute('insert into character_title_selection values (50002,2003)')
 revoke=dict(title_request,operation='revoke',request_id=secrets.token_hex(16))
 revoked=api('/titles/change',revoke,admin)
 assert revoked['selected'] is None and not next(t for t in revoked['rows'] if t['id']==2003)['owned']
 # Temporary local mode removes only the second password, never account authentication or staff roles.
 (fixture/'Config/server.json').write_text(json.dumps({'gm_local_account_login_only':True}))
 assert api('/health')['staff_password_required'] is False
 local_admin=api('/login',dict(username='GMFixture0',password=password,character='FixtureAdmin'))['token']
 api('/login',dict(username='GMFixture0',password='incorrect-account-password',character='FixtureAdmin'),deny=True)
 api('/login',dict(username='GMFixture2',password=password,character='FixtureMod'),deny=True)
 api('/logout',{},local_admin)
 (fixture/'Config/server.json').write_text(json.dumps({'gm_local_account_login_only':False}))
 api('/login',dict(username='GMFixture0',password=password,character='FixtureAdmin'),deny=True)
 items=api('/items',{},admin)['rows'];assert items
 entities=api('/entities',{'query':'berneo'},admin)['rows'];assert entities
 cash=api('/catalog/cash',{'query':'gacha'},admin)['rows'];assert cash
 offer=cash[0]
 api('/catalog/cash',{'offer':offer['offer'],'old_price':offer['price'],'new_price':offer['price']+1},gm,deny=True)
 changed=api('/catalog/cash',{'offer':offer['offer'],'old_price':offer['price'],'new_price':offer['price']+1,'query':offer['name']},admin)['rows']
 assert any(r['offer']==offer['offer'] and r['price']==offer['price']+1 for r in changed)
 api('/catalog/cash',{'offer':offer['offer'],'old_price':offer['price'],'new_price':offer['price']+2},admin,deny=True)
 prize=api('/catalog/gacha',{'tab':1},admin)['rows'][0]
 gacha_edit=dict(tab=1,position=prize['position'],old_item=prize['item'],old_quantity=prize['quantity'],old_weight=prize['weight'],old_grade=prize['grade'],new_item=prize['item'],quantity=prize['quantity'],weight=prize['weight']+1,grade=prize['grade'])
 api('/catalog/gacha',gacha_edit,gm,deny=True)
 assert api('/catalog/gacha',gacha_edit,admin)['rows'][0]['weight']==prize['weight']+1
 api('/catalog/gacha',gacha_edit,admin,deny=True)
 status=api('/status',{},admin);assert status['items']>1000 and status['placements']>1000
 player=api('/players',{},admin)['rows'];assert any(p['eid']==50001 for p in player)
 # Choose an ordinary stackable item from the real catalog; if unavailable, use any item.
 item=next((i for i in items if i['id']!=999999),None);assert item
 request=dict(request_id=secrets.token_hex(16),kind='give',target_eid=50002,items=[dict(id=item['id'],quantity=1)],enchant=0)
 api('/action',request,gm,deny=True)
 result=api('/action',request,admin);assert result['status']=='delivered',result
 replay=api('/action',request,admin);assert replay['replayed']
 inventory=api('/inventory',{'eid':50002},admin)['rows'];assert sum(i['quantity'] for i in inventory if i['id']==item['id'])==1
 gear=api('/equipment',{'class_family':100,'include_shared':False,'slot':7,'max_level':1},admin)['rows'];assert gear and all(g['class']//100==1 and g['equipment'] and g['slot']==7 for g in gear)
 for family in (100,200,300,400,500,600,800):
  filtered=api('/equipment',{'class_family':family,'include_shared':False},admin)['rows'];assert filtered and all(g['class']//100*100==family for g in filtered)
 configuration=dict(grade=18,common=[dict(stat='physical_attack_flat',value=100),dict(stat='critical_multiplier',value=.25)],craft=[dict(stat='physical_defense_flat',value=100)])
 gear_request=dict(request_id=secrets.token_hex(16),kind='give',reason='Configured equipment fixture',target_eid=50002,items=[dict(id=gear[0]['id'],quantity=1)],equipment_config=configuration)
 api('/action',gear_request,gm,deny=True);assert api('/action',gear_request,admin)['status']=='delivered'
 owned=api('/inventory',{'eid':50002,'equipment_only':True},admin)['rows'];instance=next(g for g in owned if g['id']==gear[0]['id'] and g['grade']==18)
 detail=api('/equipment/details',{'eid':50002,'uid':instance['uid']},admin);assert len(detail['affixes'])==3 and detail['grade']==18
 api('/equipment/details',{'eid':50001,'uid':instance['uid']},admin,deny=True)
 edit=dict(request_id=secrets.token_hex(16),kind='equipment',reason='Equipment edit fixture',target_eid=50002,uid=instance['uid'],item_id=instance['id'],equipment_config=dict(grade=15,common=[dict(stat='physical_attack_flat',value=150)]))
 api('/action',edit,gm,deny=True);assert api('/action',edit,admin)['status']=='applied';assert api('/action',edit,admin)['replayed']
 detail=api('/equipment/details',{'eid':50002,'uid':instance['uid']},admin);assert detail['grade']==15 and len(detail['affixes'])==2 and any(a['layer']=='craft' for a in detail['affixes'])
 for invalid in ({'grade':19},{'common':[dict(stat='physical_attack_flat',value=151)]},{'common':[dict(stat='critical_multiplier',value=.26)]},{'common':[dict(stat='attack_speed',value=11)]},{'common':[dict(stat='move_speed',value=1)]},{'common':[dict(stat='physical_attack_flat',value=1)]*3}):
  api('/action',dict(edit,request_id=secrets.token_hex(16),equipment_config=invalid),admin,deny=True)
 api('/action',dict(edit,request_id=secrets.token_hex(16),target_eid=50001),admin,deny=True)
 assert api('/equipment/details',{'eid':50002,'uid':instance['uid']},admin)['grade']==15
 request['items'][0]['quantity']=2;api('/action',request,admin,deny=True)
 mail=dict(request_id=secrets.token_hex(16),kind='mail',reason='Fixture mail validation',target_eid=50002,items=[dict(id=item['id'],quantity=1)],sender='Presente de GM',title='Fixture letter',message='Test correspondence',enchant=0)
 assert api('/action',mail,gm)['status']=='pending'
 assert api('/action',mail,gm)['replayed']
 with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:
  origins=db.execute(
   'SELECT o.uid,o.creator_name,o.recipient_name,o.method,o.quantity,i.quantity '
   'FROM gm_item_origins o JOIN inventory_slots i ON i.instance_uid=o.uid '
   'WHERE o.action_id IN (?,?) ORDER BY o.method',
   (request['request_id'],mail['request_id'])).fetchall()
  assert [row[1:] for row in origins]==[('FixtureAdmin','FixtureGM','give',1,1)],origins
  stored=db.execute('SELECT target_eid,attachments,status FROM gm_mail WHERE id=?',(mail['request_id'],)).fetchone()
  assert stored and (stored[0],json.loads(stored[1]),stored[2])==(50002,mail['items'],'pending'),stored
  assert db.execute('SELECT action_id FROM gm_item_origins WHERE uid=?',(instance['uid'],)).fetchone()==(gear_request['request_id'],)
 assert any(m['title']=='Fixture letter' for m in api('/mail',{},admin)['rows'])
 if len(sys.argv)>4:
  claim=subprocess.run([str(Path(sys.argv[4]).resolve()),str(fixture),mail['request_id']],
                       capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
  assert claim.returncode==0,claim.stdout+claim.stderr
  print(claim.stdout.strip())
 mail.update(request_id=secrets.token_hex(16),sender='Sistema');api('/action',mail,gm,deny=True)
 batch=dict(mail,request_id=secrets.token_hex(16),kind='mail_all',items=[])
 api('/action',batch,gm,deny=True)
 mass=api('/action',batch,admin);assert mass['status']=='batch_processed' and len(mass['rows'])==len(player)
 replay_batch=api('/action',batch,admin);assert all(row.get('replayed') for row in replay_batch['rows'])
 cash=dict(request_id=secrets.token_hex(16),kind='cash',reason='Fixture cash validation',target_eid=50002,amount=123)
 api('/action',cash,gm,deny=True);assert api('/action',cash,admin)['status']=='applied'
 with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:assert db.execute('SELECT balance FROM premium_wallet WHERE account_id=?',('gmfixture1',)).fetchone()[0]==123
 source=next((e for e in entities if e['map']=='berneo'),None);assert source
 spawn=dict(request_id=secrets.token_hex(16),kind='spawn',reason='Fixture native spawn',target_eid=50002,placement=source['key'],mode='original',count=1,duration=60,channel=1)
 api('/action',spawn,gm,deny=True);assert api('/action',spawn,admin)['status']=='spawn_registered'
 with sqlite3.connect(fixture/'Database/karos_gameplay.sqlite') as db:
  raw=db.execute('SELECT carrier,npc_id FROM gm_spawns').fetchone();assert int.from_bytes(bytes.fromhex(raw[0])[64:68],'little')==raw[1]
 with sqlite3.connect(fixture/'Database/karos_roles.sqlite') as db:db.execute("UPDATE staff_roles SET role='Mod' WHERE account_id='gmfixture1'")
 api('/players',{},gm,deny=True)
 api('/logout',{},admin);api('/players',{},admin,deny=True)
 print('PASS: native HTTP; account + staff authentication; GM/Admin permissions; exact catalog IDs; inventory delivery; idempotency; mail records + attachments; account Cash; native spawn identity; immediate role revocation; logout. Copied DB fixture only.')
finally:
 kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.OpenEventW.argtypes=[ctypes.c_uint32,ctypes.c_int,ctypes.c_wchar_p];kernel.OpenEventW.restype=ctypes.c_void_p;kernel.SetEvent.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
 event=kernel.OpenEventW(2,False,f'Local\\RoshGmStop_{process.pid}')
 if event:kernel.SetEvent(event);kernel.CloseHandle(event)
 try:process.wait(timeout=10)
 except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)
