"""Regressions for contradictory hashes and concurrent source-envelope changes."""
from pathlib import Path
import base64, copy, hashlib, json, sys, tempfile, unittest
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools'))
import publish_client_release as publisher

class ReleaseConsistency(unittest.TestCase):
    def setUp(self):
        self.protected={'files':[{'path':'Data/karos_025.kpk','size':17,'sha256':'a'*64}]}
        self.payload={'integrity_manifest_sha256':'c'*64,'files':[
            dict(self.protected['files'][0]),{'path':'CLIENT_PROTECTED_FILES.json','size':42,'sha256':'c'*64}]}
    def test_consistent(self):
        publisher.validate_consistency(self.payload,self.protected)
    def test_conflicting_hash(self):
        self.payload['files'][0]['sha256']='b'*64
        with self.assertRaisesRegex(ValueError,'disagree'):
            publisher.validate_consistency(self.payload,self.protected)
    def test_conflicting_size(self):
        self.payload['files'][0]['size']=18
        with self.assertRaisesRegex(ValueError,'disagree'):
            publisher.validate_consistency(self.payload,self.protected)
    def test_missing_repair_pack(self):
        self.payload['files']=self.payload['files'][1:]
        with self.assertRaisesRegex(ValueError,'missing'):
            publisher.validate_consistency(self.payload,self.protected)
    def test_static_overlay_requires_repair(self):
        self.protected['files'].append({'path':'overlay/libcef.dll','size':19,'sha256':'e'*64})
        with self.assertRaisesRegex(ValueError,'missing'):
            publisher.validate_consistency(self.payload,self.protected)
        self.payload['files'].append(dict(self.protected['files'][-1]))
        publisher.validate_consistency(self.payload,self.protected)
    def test_running_launchers_use_installer(self):
        for name in ('KarosAsmaraLauncher.exe','KarosUpdater.exe','Neo Asmara-Launcher.exe'):
            self.protected['files'].append({'path':name,'size':19,'sha256':'e'*64})
        publisher.validate_consistency(self.payload,self.protected)
    def test_duplicate_path(self):
        self.payload['files'].append(dict(self.payload['files'][0]))
        with self.assertRaisesRegex(ValueError,'Duplicate'):
            publisher.validate_consistency(self.payload,self.protected)
    def test_conflicting_protected_manifest(self):
        self.payload['files'][1]['sha256']='d'*64
        with self.assertRaisesRegex(ValueError,'integrity digest'):
            publisher.validate_consistency(self.payload,self.protected)
    def test_source_envelope_changes_during_copy(self):
        folder=Path(tempfile.mkdtemp(prefix='release-consistency-',dir=ROOT/'build'))
        source,client,dest=folder/'source',folder/'client',folder/'dest'
        (source/'blobs').mkdir(parents=True);client.mkdir()
        data=b'reviewed game data';sha=hashlib.sha256(data).hexdigest()
        protected=json.dumps({'files':[{'path':'Data/test.kpk','size':len(data),'sha256':sha}]}).encode()
        protected_sha=hashlib.sha256(protected).hexdigest()
        (client/'CLIENT_PROTECTED_FILES.json').write_bytes(protected)
        for digest,blob in ((sha,data),(protected_sha,protected)):
            (source/'blobs'/digest).write_bytes(blob)
        payload={'sequence':1,'integrity_manifest_sha256':protected_sha,'files':[
            {'path':'Data/test.kpk','sha256':sha,'size':len(data)},
            {'path':'CLIENT_PROTECTED_FILES.json','sha256':protected_sha,'size':len(protected)}]}
        envelope=json.dumps({'payload':base64.b64encode(json.dumps(payload).encode()).decode(),'signature':'fixture'}).encode()
        (source/'manifest.json').write_bytes(envelope)
        saved=(publisher.SOURCE,publisher.CLIENT,publisher.DEST,publisher.copy_verified)
        changed=False
        def copy_and_replace_source(src,dst,expected):
            nonlocal changed
            saved[3](src,dst,expected)
            if not changed:
                changed=True;(source/'manifest.json').write_bytes(b'future source release changed concurrently')
        try:
            publisher.SOURCE,publisher.CLIENT,publisher.DEST=source,client,dest
            publisher.copy_verified=copy_and_replace_source
            publisher.publish(dest)
            self.assertTrue(changed)
            self.assertEqual((dest/'manifest.json').read_bytes(),envelope)
        finally:
            publisher.SOURCE,publisher.CLIENT,publisher.DEST,publisher.copy_verified=saved

if __name__=='__main__':unittest.main()
