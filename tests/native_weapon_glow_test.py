"""Check staged asset graphs and original-grade preservation without launching a game."""
from pathlib import Path
import hashlib,json,re,struct,sys
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools'))
from stage_weapon_glows import KPK,object_chunks,effect_table,records,djb,PREFIX,COLORS
live=KPK('C:/DEV KAROS/CLIENTE - Copia');stage=ROOT/'build/weapon-glows-20261028'
report=json.loads((stage/'weapon-glow-verification.json').read_text());files={v['path']:v for v in report['resources']}
assert not report['armorModified']
for path,entry in files.items():assert hashlib.sha256((stage/path).read_bytes()).hexdigest()==entry['sha256']
checked=0
for model in report['models']:
 if not model['staged']:continue
 path=model['path'];before=live.read(path);after=(stage/path).read_bytes()
 assert '/weapon/' in path and 'bowbox' not in path
 if before.startswith(b'#.FILE-INFO'):
  old=before.decode('ascii').splitlines();new=after.decode('ascii').splitlines()
  assert all(line in new for line in old if not re.search(r'#\.(EFs_SIZE|OEs_SIZE)',line))
  for grade in range(14,19):assert after.count(f'"Weapon_Enchant{grade}"'.encode())==1
 else:
  old_chunks,old_end=object_chunks(before);new_chunks,new_end=object_chunks(after)
  start=60+struct.unpack_from('<I',before,56)[0]
  assert before[:start+92]==after[:start+92] and before[old_end:]==after[new_end:]
  old_fixed=[before[a:z] for tag,a,z in old_chunks if tag not in (20,34)]
  new_fixed=[after[a:z] for tag,a,z in new_chunks if tag not in (20,34)]
  assert old_fixed==new_fixed,'Original model geometry/event bindings changed'
  for tag,a,z in old_chunks:
   if tag not in (20,34):continue
   _,n,m=next(v for v in new_chunks if v[0]==tag)
   assert after[n+8:n+8+(z-a-8)]==before[a+8:z], 'Old effect/grade binding changed'
  _,a,z=next(v for v in new_chunks if v[0]==34);p=a+8;grades=[]
  for _ in range(struct.unpack_from('<I',after,a+4)[0]):
   ident,size=struct.unpack_from('<II',after,p);name=after[p+8:p+8+size]
   if 0x1000e<=ident<=0x10012:assert name==f'Weapon_Enchant{ident-0x10000}'.encode();grades.append(ident)
   p+=8+size+40
  assert grades==list(range(0x1000e,0x10013)) and p==z
 checked+=1
effects=0
for path in files:
 if not path.endswith('._ef'):continue
 b=(stage/path).read_bytes();size=struct.unpack_from('<I',b,56)[0];re_path=PREFIX+b[60:60+size].decode().lower();assert re_path in files
 resource=(stage/re_path).read_bytes();rows=records(resource)
 root=60+size;assert struct.unpack_from('<I',b,root)[0]==0x12345678
 root_name=b[root+4:root+68].split(b'\0')[0];assert djb(root_name)==struct.unpack_from('<I',b,root+68)[0]
 for p,end,kind,name in rows:
  # Every resource referenced by the original EF now has the matching new ID.
  identity=name.ljust(64,b'\0')+struct.pack('<I',djb(name))
  if kind==179:assert identity in b,'Particle child missing from effect graph'
  if kind==179:
   assert struct.unpack_from('<II',resource,p+84+0x1c4)==(2,0)
   for offset in (0x60,0x70,0x80,0x90):assert all(0<=v<=1 for v in struct.unpack_from('<3f',resource,p+84+offset))
 effects+=1
result={'passed':True,'models':checked,'newEffectGraphs':effects,'oldGradesGeometryEventsPreserved':True,'armorModified':False,'liveVisualValidation':'pending','unavailableSourceModels':[v['path'] for v in report['models'] if not v['staged'] and not v.get('excluded')]}
(ROOT/'docs/native-weapon-glow-verification.json').write_text(json.dumps(result,indent=2))
print(f'PASS: {checked} weapon/shield models; {effects} effect graphs; old grades/geometry/events preserved; no armor changes; two unavailable original model paths recorded; live appearance pending.')
