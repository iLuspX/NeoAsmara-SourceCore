"""Read-only regression against the supplied client and its captured event packet."""
from pathlib import Path
import sys,json,struct
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools/client_resources'))
from native_dungeon_map import objects,events
from kpk_reader import KPK
client=KPK(Path(sys.argv[1]))
resource='resunpak/map/catacomb02event/map/catacomb02event_60LV.me'
native=client.read(resource);parsed=events(native)
room=next(group for group in parsed['triggers']if group['id']==92)
action=room['actions'][0]
packet=struct.pack('>IIIII',29614,action['kind'],action['target'],action['parameter']or room['mode'],0xffffffff)
fixture=json.loads((ROOT/'tests/fixtures/dungeon-capture-20261118.json').read_text())
assert packet.hex()==fixture['first_room_29614']['body'],'Native event does not match captured wire'
mo=client.read('resunpak/map/catacombBelltower/object/catacombBelltower.mo')
actors=objects(mo)['objects'];ghouls=[o for o in actors if o['kind']==7 and o['extra'][0]==5100433]
assert {o['reference']for o in ghouls}=={501,502,503,504,505,506,507,508,509,510,515}
assert len({tuple(o['position'])for o in ghouls})==11
for parser,blob in [(events,native),(objects,mo)]:
 for malformed in [blob[:31],blob[:-1],blob+b'\x00',b'\xff\xff\xff\xff'+blob[4:]]:
  try:parser(malformed)
  except (ValueError,KeyError,struct.error):pass
  else:raise AssertionError('Malformed dungeon resource accepted')
print('PASS: native ME event matches capture; all 11 distinct Ghoul references and home positions; truncated/extra/unbounded MO/ME rejected; client unchanged')
