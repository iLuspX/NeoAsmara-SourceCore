param([string]$ToolchainRoot='', [switch]$SkipTests)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$metadata=Get-Content -LiteralPath (Join-Path $root 'tools/native_toolchain.json') -Raw | ConvertFrom-Json
if(-not $ToolchainRoot){$ToolchainRoot=Join-Path $root ('tools/toolchains/llvm-mingw-'+$metadata.compiler_release+'-ucrt-x86_64')}
$compiler=Join-Path $ToolchainRoot 'bin/x86_64-w64-mingw32-clang++.exe'
if(-not(Test-Path -LiteralPath $compiler)){
 & python (Join-Path $root 'tools/bootstrap_toolchain.py')
 if($LASTEXITCODE){throw 'Compiler setup failed. Python 3.11 or newer is required for the download helper.'}
 if(-not(Test-Path -LiteralPath $compiler)){throw 'Compiler was not found at the requested ToolchainRoot.'}
}
$build=Join-Path $root 'build/native'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$flags=@('-std=c++17','-O1','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0A00','-I',(Join-Path $root 'include'),'-I',(Join-Path $root 'third_party'))
$sources=@(Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter 'native_*.cpp' -File | Where-Object {$_.BaseName -notin @('native_entry','native_data_tool','native_gm_service')})
$exports=[regex]::Matches((($sources | ForEach-Object {Get-Content -LiteralPath $_.FullName -Raw}) -join "`n"),'sqlite3_[A-Za-z0-9_]+(?=\s*\()') | ForEach-Object {$_.Value} | Sort-Object -Unique
$definition=Join-Path $build 'winsqlite3.def'
[IO.File]::WriteAllLines($definition,@('LIBRARY winsqlite3.dll','EXPORTS')+$exports,[Text.Encoding]::ASCII)
$sqlite=Join-Path $build 'libwinsqlite3.a'
& (Join-Path $ToolchainRoot 'bin/llvm-dlltool.exe') '-m' 'i386:x86-64' '-d' $definition '-l' $sqlite
if($LASTEXITCODE){throw 'SQLite import library generation failed.'}
$objects=@{}
foreach($source in $sources){
 $object=Join-Path $build ($source.BaseName+'.o')
 Write-Host ('Compiling '+$source.Name)
 & $compiler @flags '-c' $source.FullName '-o' $object
 if($LASTEXITCODE){throw ('Compilation failed: '+$source.Name)}
 $objects[$source.BaseName]=$object
}
$foundation=@('native_core','native_accounts','native_wire' | ForEach-Object {$objects[$_]})
$game=@($objects.Keys | Where-Object {$_ -notin @('native_application','native_services','native_gm')} | ForEach-Object {$objects[$_]})
$libraries=@($sqlite,'-lbcrypt','-lcrypt32','-lws2_32')
$runtime=Join-Path $build 'KarosRuntime.dll'
$import=Join-Path $build 'libKarosRuntime.dll.a'
& $compiler '-shared' @($objects.Values) @libraries '-lcomctl32' '-lshell32' '-ldbghelp' '-lgdi32' '-o' $runtime ('-Wl,--out-implib='+$import)
if($LASTEXITCODE){throw 'Runtime link failed.'}
$index=0
foreach($role in @('LoginServer','MainServer','LobbyServer','WorldServer')){
 & $compiler @flags '-municode' '-mwindows' ('-DROSH_ROLE_ID='+$index) (Join-Path $root 'src/native_entry.cpp') $import '-o' (Join-Path $build ($role+'.exe'))
 if($LASTEXITCODE){throw ('Service link failed: '+$role)}
 $index++
}
foreach($program in @(@('KarosDataTool','src/native_data_tool.cpp'),@('RoshGmService','src/native_gm_service.cpp'),@('RoshAccountService','tools/distribution/account_service.cpp'))){
 $inputs=if($program[0] -eq 'KarosDataTool' -or $program[0] -eq 'RoshAccountService'){$foundation}else{@($game)+$objects['native_gm']+$objects['native_services']}
 & $compiler @flags '-municode' (Join-Path $root $program[1]) @inputs @libraries '-o' (Join-Path $build ($program[0]+'.exe'))
 if($LASTEXITCODE){throw ('Program link failed: '+$program[0])}
}
& $compiler @flags '-municode' (Join-Path $root 'tools/distribution/server_control.cpp') '-liphlpapi' '-lwinhttp' '-lws2_32' '-o' (Join-Path $build 'ServerControl.exe')
if($LASTEXITCODE){throw 'Server controller link failed.'}
foreach($dll in @('libc++.dll','libunwind.dll')){Copy-Item -LiteralPath (Join-Path $ToolchainRoot ('x86_64-w64-mingw32/bin/'+$dll)) -Destination $build -Force}
if(-not $SkipTests){
 & $compiler @flags (Join-Path $root 'tests/native_foundation_test.cpp') @foundation @libraries '-o' (Join-Path $build 'KarosNativeTests.exe')
 if($LASTEXITCODE){throw 'Foundation test link failed.'}
 & (Join-Path $build 'KarosNativeTests.exe')
 if($LASTEXITCODE){throw 'Foundation tests failed.'}
 foreach($test in @(@('KarosDungeonTests','native_dungeons_test.cpp'),@('KarosDungeonFlowTests','native_dungeon_flow_test.cpp'),@('KarosDungeonDoorReplayTests','native_dungeon_door_replay_test.cpp'),@('KarosAllDungeonResourcesTests','native_all_dungeon_resources_test.cpp'))){
  & $compiler @flags '-municode' (Join-Path $root ('tests/'+$test[1])) @game @libraries '-o' (Join-Path $build ($test[0]+'.exe'))
  if($LASTEXITCODE){throw ('Dungeon test link failed: '+$test[0])}
 }
}
Write-Host ('Build complete: '+$build)
Write-Host 'No running server, client files, accounts or databases were changed.'
