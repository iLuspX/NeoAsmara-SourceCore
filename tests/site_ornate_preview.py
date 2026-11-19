"""Own-site headless visual/interaction QA; no desktop or account access."""
from pathlib import Path
import http.server,threading,urllib.request,urllib.error,urllib.parse,json,subprocess,time
ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'build/site-ornate-preview';OUT.mkdir(exist_ok=True)
UPSTREAM='http://127.0.0.1:18190';reports={};blocked=[]
boot=r"""
window.qaErrors=[];window.addEventListener('error',e=>window.qaErrors.push(String(e.message)));
window.addEventListener('securitypolicyviolation',e=>window.qaErrors.push('CSP '+e.blockedURI+' '+e.violatedDirective));
window.addEventListener('load',async()=>{const key=new URLSearchParams(location.search).get('qa')||'page';try{
 const assert=(v,m)=>{if(!v)throw Error(m)};
 for(let i=0;i<100;i++){if(document.querySelector('#home-news .news-card')&&document.querySelector('#cash-quantity'))break;await new Promise(r=>setTimeout(r,100));}
 await new Promise(r=>setTimeout(r,500));assert(document.querySelector('#load-error').hidden,'Portal load error');
 assert(document.title.includes('ROSH : Neo Asmara'),'Project name missing');const logo=document.querySelector('.nav .project-logo');assert(logo&&logo.complete&&logo.naturalWidth>0,'Project logo missing');
 assert(getComputedStyle(document.querySelector('.realm-panel')).borderRadius==='19px','New panels absent');
 assert(document.querySelectorAll('.realm-carving').length>20,'Carved ornaments absent');
 const home=location.pathname==='/';if(home&&innerWidth>1180)assert(getComputedStyle(document.querySelector('.realm-layout')).gridTemplateColumns.split(' ').length===3,'Desktop columns lost');
 const visible=document.querySelector('[data-view]:not([hidden])');assert(visible,'No route visible');
 if(location.pathname==='/loja'){
  const amount=document.querySelector('#cash-quantity');amount.value='1000';amount.dispatchEvent(new Event('input',{bubbles:true}));
  assert(document.querySelector('#checkout-total').textContent.replace(/\D/g,'')==='1090','Cash bonus changed');
  assert(document.querySelector('#checkout-price').textContent.includes('20,00'),'Cash price changed');
  assert(document.querySelectorAll('#payment-provider option').length===2,'Payment choices lost');
  amount.scrollIntoView({block:'center'});const r=amount.getBoundingClientRect();assert(document.elementFromPoint(r.x+r.width/2,r.y+r.height/2)===amount,'Input covered by ornament');
 }
 if(location.pathname==='/conta'){
  document.querySelector('#mode-register').click();assert(!document.querySelector('#confirm-label').hidden,'Register tab broken');document.querySelector('#mode-login').click();assert(document.querySelector('#confirm-label').hidden,'Login tab broken');
 }
 assert(document.documentElement.scrollWidth<=innerWidth+2,'Horizontal overflow '+document.documentElement.scrollWidth+'/'+innerWidth);
 assert(qaErrors.length===0,qaErrors.join(';'));
 if(key.startsWith('spanish')&&location.pathname==='/loja')assert(document.querySelector('.cash-instructions h2').textContent==='Tu recarga, paso a paso.','New Spanish instructions not translated');
 scrollTo(0,0);await fetch('/__qa/result',{method:'POST',body:JSON.stringify({key,passed:true,path:location.pathname,width:innerWidth,carvings:document.querySelectorAll('.realm-carving').length,scrollWidth:document.documentElement.scrollWidth})});
 }catch(e){await fetch('/__qa/result',{method:'POST',body:JSON.stringify({key,passed:false,error:String(e),errors:qaErrors})});}});
"""
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  if self.path!='/__qa/result':blocked.append(self.path);self.send_error(403);return
  data=json.loads(self.rfile.read(int(self.headers['Content-Length'])));reports[data['key']]=data;self.send_response(200);self.end_headers()
 def do_GET(self):
  path=self.path.split('?',1)[0]
  if path=='/__qa/frame':
   target=urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)['target'][0]
   data=('<!doctype html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#090e13"><iframe title="390px responsive preview" style="width:390px;height:1250px;border:0;display:block" src="'+target.replace('&','&amp;')+'"></iframe></body></html>').encode();status=200;headers={'Content-Type':'text/html; charset=utf-8'}
  elif path=='/__qa/boot.js':data=boot.encode();status=200;headers={'Content-Type':'text/javascript; charset=utf-8'}
  elif path in ('/fantasy.css','/realm.js','/i18n.js','/portal.js','/guide.js','/assets/rosh-logo.png'):
   data=(ROOT/'web_site'/path.lstrip('/')).read_bytes();status=200;headers={'Content-Type':'image/png' if path.endswith('.png') else ('text/css; charset=utf-8' if path.endswith('.css') else 'text/javascript; charset=utf-8')}
  else:
   try:response=urllib.request.urlopen(UPSTREAM+self.path,timeout=15)
   except urllib.error.HTTPError as e:response=e
   status=response.status;headers=dict(response.headers);data=response.read()
   if headers.get('Content-Type','').startswith('text/html'):
    data=(ROOT/'web_site/portal.html').read_bytes()
    if '-390' in self.path:headers['Content-Security-Policy']=headers.get('Content-Security-Policy','').replace("frame-ancestors 'none';",'')
    data=data.replace(b'</head>',b'<script src="/__qa/boot.js" defer></script></head>')
  self.send_response(status)
  for key,value in headers.items():
   if key.lower() not in ('content-length','connection','server','date','transfer-encoding'):self.send_header(key,value)
  self.send_header('Content-Length',str(len(data)));self.end_headers()
  try:self.wfile.write(data)
  except (ConnectionResetError,BrokenPipeError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
browser='C:/Program Files (x86)/Microsoft/EdgeCore/155.0.4283.62/msedge.exe'
scenarios=[('home-desktop','/',1440,1250),('cash-desktop','/loja',1440,1100),('account-desktop','/conta',1440,1100),('guide-desktop','/guia/habilidades',1440,1100),('terms-desktop','/termos',1440,1100),('home-mobile','/',390,1250),('cash-mobile','/loja',390,1250),('guide-mobile','/guia',390,1250),('spanish-home','/',1440,1250),('spanish-cash','/loja',1440,1100),('home-390','/',500,1250),('cash-390','/loja',500,1250),('guide-390','/guia',500,1250)]
try:
 for key,path,w,h in scenarios:
  with (OUT/(key+'.log')).open('w',encoding='utf-8') as log:
   language='es' if key.startswith('spanish') else 'pt'
   target=f'{path}?lang={language}&qa={key}'
   if key.endswith('-390'):target='/__qa/frame?target='+urllib.parse.quote(target,safe='')
   command=[browser,'--headless','--no-first-run','--no-default-browser-check','--disable-extensions','--disable-background-networking','--hide-scrollbars',f'--user-data-dir={OUT/(key+"-profile")}',f'--window-size={w},{h}','--virtual-time-budget=16000',f'--screenshot={OUT/(key+".png")}',f'http://127.0.0.1:{server.server_port}{target}']
   subprocess.run(command,stdout=log,stderr=log,timeout=55,creationflags=subprocess.CREATE_NO_WINDOW)
  result=reports.get(key,{'key':key,'passed':False,'error':'No browser result'});print(json.dumps(result),flush=True)
finally:server.shutdown()
report={'passed':len(reports)==len(scenarios) and all(r['passed'] for r in reports.values()),'pages':list(reports.values()),'blockedPosts':blocked,'desktopAccess':False,'realAccountsUsed':False,'paymentsSubmitted':False}
(OUT/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8');raise SystemExit(0 if report['passed'] else 1)
