"""Exercise our own overlay in an isolated headless browser, no desktop access."""
from pathlib import Path
import http.server,threading,json,subprocess,shutil,time
ROOT=Path(__file__).resolve().parent.parent
LIVE=Path('C:/DEV KAROS/CLIENTE - Copia/overlay/ui')
OUT=ROOT/'build/cash-overlay-headless';OUT.mkdir(exist_ok=True)
catalog=json.loads((ROOT/'ReferenceOverrides/data/cash_shop_local.json').read_text(encoding="utf-8"))
items=json.loads((ROOT/'ReferenceOverrides/data/item_catalog_verified.json').read_text(encoding="utf-8"));names={i['id']:i['name'] for i in items}
articles=[]
for a in catalog['offers']:
 if a.get('unavailable'):continue
 entries=[[names.get(i,str(i)),n,i] for i,n in a['items']]
 articles.append([a['id'],a['section'],a['title'],a['cash']*100,0,a['picture'],[[0,entries]],a['about'],{'class':a.get('class',0),'level':a.get('level',0),'mode':a['purchase_mode'],'boxed':a['boxed'],'counterpart':a.get('counterpart',0),'productGroup':a.get('product_group',0),'duration':a.get('duration_seconds',0),'displayTitle':a.get('display_title','')}])
fixture={'sections':catalog['sections'],'articles':articles}
result={};errors=[]
boot=r'''
window.karos={send:s=>(window.sent||(window.sent=[])).push(s)};
window.addEventListener('error',e=>{window.pageError=String(e.error||e.message)});
window.addEventListener('load',async()=>{try{
 const f=await(await fetch('/fixture.json')).json();const assert=(v,m)=>{if(!v)throw Error(m)};
 const choose=(selector,value)=>{const root=document.querySelector(selector);assert(root,'Missing control '+selector);assert(root.tagName!=='SELECT','Invisible native popup '+selector);root.querySelector('.value').click();const option=[...root.querySelectorAll('.list>div')].find(x=>x.dataset.value===String(value));assert(option,'Missing option '+selector+' '+value);option.scrollIntoView({block:'nearest'});const rect=option.getBoundingClientRect();assert(rect.height>0,'Option not rendered');const target=document.elementFromPoint(rect.left+rect.width/2,rect.top+rect.height/2);assert(target&&option.contains(target),'Option blocked by the layout: '+selector+' '+value);option.click();};
 const receive=window.karosReceive;
 receive({cmd:'shop.balance',cents:2000000000});receive({cmd:'shop.sections',version:'test',playerClass:600,front:6,sections:f.sections,parts:1});receive({cmd:'shop.articles',version:'test',part:0,articles:f.articles});receive({cmd:'open',page:'shop',lang:'pt'});
 assert(!window.pageError,window.pageError);assert(document.querySelectorAll('.good').length>50,'Catalogue missing');
 let filters=document.querySelectorAll('#sort-host .asmara-shop-filter');assert(filters.length===2,'Class/level filters absent');assert(filters[0].dataset.value==='0','All-class catalogue not selected');
 const card=[...document.querySelectorAll('.good')].find(c=>f.articles.find(x=>x[0]===Number(c.dataset.id))[8].boxed);assert(card,'No complete costume selectable');card.click();const id=Number(card.dataset.id);const a=f.articles.find(x=>x[0]===id);assert(a[8].boxed,'Costume not selectable');
 document.querySelector('[data-purchase-mode="boxed"]').click();document.querySelector('.pick-buy').click();document.querySelector('#confirm-yes').click();assert(window.sent.includes('npcshop.cashbuy '+a[8].counterpart+' 1'),'Boxed purchase wrong SKU');
 receive({cmd:'shop.bought',ok:true,cents:2000000000});document.querySelector('[data-purchase-mode="direct"]').click();document.querySelector('.pick-buy').click();document.querySelector('#confirm-yes').click();assert(window.sent.includes('npcshop.cashbuy '+id+' 1'),'Direct purchase wrong SKU');receive({cmd:'shop.bought',ok:true,cents:2000000000});
 choose('[data-filter=class]',200);const foreign=[...document.querySelectorAll('.good')].find(c=>f.articles.find(x=>x[0]===Number(c.dataset.id))[8].class===200);assert(foreign,'Foreign class catalogue missing');foreign.click();assert(!document.querySelector('.pick-buy').disabled,'Foreign class gift purchase blocked');
 choose('[data-filter=class]',600);choose('[data-filter=level]','40');assert([...document.querySelectorAll('.good')].every(c=>f.articles.find(x=>x[0]===Number(c.dataset.id))[8].level===40),'Level filter leaked other levels');assert(document.querySelectorAll('.good').length>0,'Level filter lost catalogue');
 receive({cmd:'open',page:'shop',lang:'es'});assert(document.body.textContent.includes('Pociones Cash')&&document.body.textContent.includes('cuatro piezas'),'Spanish sections absent');receive({cmd:'open',page:'shop',lang:'en'});assert(document.body.textContent.includes('Cash potions')&&document.body.textContent.includes('four pieces'),'English sections absent');receive({cmd:'open',page:'shop',lang:'pt'});
 const potionGroup=[...document.querySelectorAll('.cat.grp')].find(e=>e.textContent.includes('Poções Cash'));assert(potionGroup,'Cash potion category missing');potionGroup.click();
 const hpCategory=document.querySelector('[data-section="2:hp"]');assert(hpCategory,'HP subcategory missing');hpCategory.click();
 assert(document.querySelectorAll('.good').length>0,'HP potions disappeared');assert([...document.querySelectorAll('.good')].every(c=>/health|healing/i.test(f.articles.find(x=>x[0]===Number(c.dataset.id))[2])),'HP category contains unrelated potions');
 const costumeGroup=[...document.querySelectorAll('.cat.grp')].find(e=>e.textContent.includes('Trajes e aparências'));assert(costumeGroup,'Costume group missing');costumeGroup.click();document.querySelector('[data-section="6"]').click();
 const finalCard=[...document.querySelectorAll('.good')].find(c=>f.articles.find(x=>x[0]===Number(c.dataset.id))[8].boxed);if(finalCard)finalCard.click();
 assert(document.querySelector('.asmara-native-shop-icon'),'Missing native cash icon');
 const product=[...document.querySelectorAll('.good')].find(c=>{
   const a=f.articles.find(x=>x[0]===Number(c.dataset.id));
   return a[8].productGroup && f.articles.some(v=>v[8].productGroup===a[8].productGroup && v[8].duration>0);
 });
 assert(product,'No product with time-limited variants');product.click();
 const term=document.querySelector('#cash-duration');assert(term,'Rental variants were not grouped');
 const selectedProduct=f.articles.find(x=>x[0]===Number(product.dataset.id));const rental=f.articles.find(x=>x[8].productGroup===selectedProduct[8].productGroup&&x[8].mode!=='boxed'&&x[8].duration>0);
 assert(rental,'Missing rental selection');choose('#cash-duration',rental[0]);
 document.querySelector('.pick-buy').click();document.querySelector('#confirm-yes').click();
 assert(window.sent.includes('npcshop.cashbuy '+rental[0]+' 1'),'Wrong rental SKU submitted');
 const translated=window.ASMARA_ITEM_LABELS.pt['1000900020'];assert(translated&&translated.includes('Torso'),'Native selector translation missing');
 await fetch('/result',{method:'POST',body:JSON.stringify({passed:true,offers:f.articles.length,classAndLevel:true,directAndBoxed:true,foreignClassPurchasable:true,languages:['pt','es','en'],requests:window.sent.filter(x=>x.startsWith('npcshop.cashbuy'))})});
 }catch(e){await fetch('/result',{method:'POST',body:JSON.stringify({passed:false,error:String(e),stack:e.stack,pageError:window.pageError})});}});
'''
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  result.update(json.loads(self.rfile.read(int(self.headers['Content-Length']))));self.send_response(200);self.end_headers()
 def do_GET(self):
  name=self.path.split('?',1)[0].lstrip('/') or 'index.html'
  if name=='fixture.json':data=json.dumps(fixture).encode();mime='application/json'
  elif name=='test_boot.js':data=boot.encode();mime='application/javascript'
  else:
   p=ROOT/'client_overlay'/name
   if not p.is_file():p=LIVE/name
   if not p.is_file():self.send_error(404);return
   data=p.read_bytes();mime={'.html':'text/html','.js':'application/javascript','.css':'text/css','.webp':'image/webp','.png':'image/png','.svg':'image/svg+xml'}.get(p.suffix,'application/octet-stream')
   if name=='index.html':data=data.replace(b'<script src="app.js">',b'<script src="test_boot.js"></script><script src="app.js">')
  self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)));self.end_headers()
  try:self.wfile.write(data)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
browser=max(Path('C:/Program Files (x86)/Microsoft/EdgeCore').glob('*/msedge.exe'),key=lambda p:tuple(map(int,p.parent.name.split('.'))))
with (OUT/'browser.log').open('w') as log:
 run=subprocess.run([str(browser),'--headless','--no-first-run','--no-default-browser-check','--disable-extensions','--disable-background-networking','--disable-brave-update',f'--user-data-dir={OUT/"edge-profile"}', '--window-size=1440,1000','--virtual-time-budget=15000',f'--screenshot={OUT/"shop-preview.png"}',f'http://127.0.0.1:{server.server_port}/'],stdout=log,stderr=log,timeout=60,creationflags=subprocess.CREATE_NO_WINDOW)
server.shutdown();(OUT/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result));raise SystemExit(0 if result.get('passed') else 1)
