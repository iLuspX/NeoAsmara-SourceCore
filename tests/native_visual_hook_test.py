"""Execute only the small staged formatter stub inside an x86 emulator.
Never launches, maps into, or controls the user's game process.
"""
from pathlib import Path
import json,struct,sys
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'build/analysis-python'))
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EAX
stage=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'build/affix-visuals-20261028/native';report=json.loads((stage/'visual-hook-verification.json').read_text());binary=(stage/'AMo.exe').read_bytes();checks=[]
for shift in (0,0x02000000):
 for operation in (0,1,12,136,137,138,500,0xffffffff):
  vm=Uc(UC_ARCH_X86,UC_MODE_32);entry=report['hookVA']+shift;target=report['codeVA']+shift;stack=0x21000800;end=0x20000000
  vm.mem_map(entry&~4095,4096);vm.mem_map(target&~4095,4096);vm.mem_map(stack&~4095,4096);vm.mem_map(end,4096)
  vm.mem_write(entry,binary[report['hookFileOffset']:report['hookFileOffset']+5]);vm.mem_write(target,bytes.fromhex(report['codeBytes']));vm.mem_write(stack,struct.pack('<II',end,operation));vm.reg_write(UC_X86_REG_ESP,stack)
  reached=[]
  def stop(uc,address,size,user):
   if address in (entry+5,end):reached.append(address);uc.emu_stop()
  vm.hook_add(UC_HOOK_CODE,stop);vm.emu_start(entry,end+1,count=30)
  labels={int(k):v.encode()+b'\0' for k,v in report.get('labels',{'137':'Mana absorption'}).items()}
  if operation in labels:
   assert reached==[end] and vm.reg_read(UC_X86_REG_ESP)==stack+4
   address=vm.reg_read(UC_X86_REG_EAX);assert bytes(vm.mem_read(address,len(labels[operation])))==labels[operation]
  else:assert reached==[entry+5] and vm.reg_read(UC_X86_REG_EAX)==((operation-1)&0xffffffff) and vm.reg_read(UC_X86_REG_ESP)==stack
  checks.append({'operation':operation,'imageShift':shift,'passed':True})
result={'passed':True,'checks':checks,'clientStarted':False,'actualGameVisualValidation':'pending'}
(ROOT/'docs/native-visual-hook-emulation.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print('PASS: 16 x86 formatter cases; unchanged original-operation path; new display label; stack balance; rebased image.')
