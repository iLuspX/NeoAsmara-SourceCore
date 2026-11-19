"""Regression for the actual observed loader alias/checklist disagreement."""
from pathlib import Path
import sys,hashlib
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'tools'))
import native_integrity as integrity
class Files:
 def __init__(self):
  self.data={};self.paths=[f'/ResUnpak/Test/item{i:03}.lod_rus' for i in range(760)]
  for name in self.paths:self.data[name]=b'original base table'
  self.data['lang/pt/'+self.paths[0].lstrip('/').lower()]=b'Portuguese translated table'
  self.data['lang/es/'+self.paths[0].lstrip('/').lower()]=b'Spanish translated table'
  for lang in ('en','pt','es'):self.list(lang)
 def resolved(self,lang,path):
  alias='lang/'+lang+'/'+path.lstrip('/').lower()
  return alias if alias in self.data else path
 def list(self,lang):
  rows=['760']+[path+'\t'+hashlib.md5(self.data[self.resolved(lang,path)]).hexdigest() for path in self.paths]
  self.data['lang/'+lang+'/checkfilelistw.ten']=integrity.encode(('\r\n'.join(rows)+'\r\n').encode())
 def read(self,name):return self.data[name]
 def entry(self,name):
  value=self.data[name];return (41,0,hash(value),len(value))
files=Files();prior=integrity.KPK;integrity.KPK=lambda _:files
try:
 rows=integrity.audit_localized(Path('.'));assert len(rows)==3 and all(r['checks']==760 and not r['mismatches'] for r in rows)
 # The old checker could pass English while the native Portuguese client failed.
 name='lang/pt/checkfilelistw.ten';correct=files.data[name];plain=integrity.decode(correct).decode();good=hashlib.md5(b'Portuguese translated table').hexdigest();wrong=hashlib.md5(b'original base table').hexdigest();files.data[name]=integrity.encode(plain.replace(good,wrong).encode())
 rows=integrity.audit_localized(Path('.'));assert not rows[0]['mismatches'] and rows[1]['mismatches'][0]['resolved'].startswith('lang/pt/') and not rows[2]['mismatches']
 files.data[name]=correct;del files.data['lang/es/checkfilelistw.ten'];rows=integrity.audit_localized(Path('.'));assert rows[2]['checks']==0 and rows[2]['mismatches']
 files.list('es');files.data['lang/pt/'+files.paths[0].lstrip('/').lower()]=b'corrupted translated table';rows=integrity.audit_localized(Path('.'));assert rows[1]['mismatches']
finally:integrity.KPK=prior
print('PASS: all3 native locale resolutions; stale PT checksum rejected despite valid EN; missing ES rejected; damaged localized bytes rejected; no checks bypassed.')
