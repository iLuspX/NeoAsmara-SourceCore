from pathlib import Path
import sys,unittest,struct
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from analyze_game_capture import reassemble
def frame(cat,sub,body):return struct.pack('>I',len(body)+6)+bytes((cat,sub,0,0,0,1))+body
class CaptureReassembly(unittest.TestCase):
 def test_fragments_coalescing_and_retransmission(self):
  a=frame(4,6,b'\0\0\x12\x34\x01\x23');b=frame(5,4,b'\0'*25);allbytes=a+b
  segments=[(1,1,.1,allbytes[:7]),(8,2,.2,allbytes[7:20]),(8,3,.3,allbytes[7:20]),(21,4,.4,allbytes[20:])]
  result=list(reassemble(segments));self.assertEqual([(p['category'],p['sub']) for p in result[:-1]],[(4,6),(5,4)]);self.assertEqual(result[-1]['reassembly'],{'gaps':[],'trailingBytes':0})
 def test_gap_does_not_invent_a_completed_packet(self):
  a=frame(4,6,b'123456');b=frame(5,4,b'\0'*25)
  result=list(reassemble([(1,1,.1,a[:8]),(100,2,.2,b)]));self.assertEqual(len(result),2);self.assertEqual(result[0]['category'],5);self.assertTrue(result[-1]['reassembly']['gaps'])
 def test_truncated_tail_is_reported(self):
  p=frame(4,6,b'123456');result=list(reassemble([(1,1,.1,p[:-1])]))
  self.assertEqual(len(result),1);self.assertEqual(result[0]['reassembly']['trailingBytes'],len(p)-1)
if __name__=='__main__':unittest.main()
