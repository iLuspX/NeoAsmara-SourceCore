"""Check installed launchers without creating accounts or starting the game."""
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

core = Path('C:/DEV KAROS/Karos Server ((Test Functions))')
archive = Path('C:/DEV KAROS/Karos Server ((Test Functions))/Integracao/NeoAsmara-DOIS-LAUNCHERS-CORE-CPP.zip')
results = []
with zipfile.ZipFile(archive) as package:
    for folder in ('CLIENTE', 'CLIENT NEO ASMARA - C++'):
        client = Path('C:/DEV KAROS') / folder
        for name, mode in (('KarosAsmaraLauncher.exe', 'dashboard'), ('KarosUpdater.exe', 'ingame'), ('Neo Asmara-Launcher.exe', 'ingame')):
            executable = client / name
            assert hashlib.sha256(executable.read_bytes()).digest() == hashlib.sha256(package.read(name)).digest()
            process = subprocess.run([str(executable), '--core-check'], timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            assert process.returncode == 0
            report = json.loads((client / 'core-integration-check.json').read_text())
            assert report['passed'] and report['login_mode'] == mode and report['signed_update_verified']
            results.append(dict(client=str(client), executable=name, **report))
    settings = json.loads(package.read('Launcher.settings.json'))
    assert Path(settings['ServerRoot']) == core
    assert Path(settings['LoginDb']) == core / 'Database/karos_login.sqlite'
output = dict(passed=True, installedLaunchers=results, archive=str(archive), actualGameplayValidated=False)
(core / 'Logs/client-launcher-integration.json').write_text(json.dumps(output, indent=2), encoding='utf-8')
print('PASS: 6 installed launcher entries, both compiled modes, C++ gate, shared account database, signed update manifest and ZIP binaries.')
