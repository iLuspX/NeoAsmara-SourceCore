# Dungeon review - 19 November 2026

## Changes in this source copy

- Native NPC entrances take priority over scene default-region markers. In Peltrok, Gelmenco's entrance stays at X=3601, Y=3045, Z=857 from `mapWarpTargetPos`. The previous override moved players to an interior marker at roughly 2533, 3645, 784. A marker is now only a fallback for missing coordinates, or missing height when the marker is within 40 map units of the native entrance. An observed arrival keeps priority.
- `native_map_destinations` uses the same fallback rule, so administrative map selection agrees with NPC travel.
- Dungeon event rows now retain their native subcode. Peltrok's 137 distinct `0A/04` maze-object initialization messages are recorded once for each new run and delivered on map ready. Joining party members and reconnecting members receive that run's recorded state without creating another initialization batch.
- Capture derivation no longer drops every room-clear packet received within 1.5 seconds of a trigger. It excludes only matching trigger responses. This restores the first room-clear condition in the level-50 belltower instead of requiring enemies behind its first door.
- The basic catacomb event dungeon now includes the missing trigger 5, recovered from the paired client request and server response in the supplied capture. It keeps room-clear prerequisites and reach checks.

Changes belong in the Core and its authenticated server rules. The client executable, resource packs and launcher checks are unchanged by this review.

## Evidence and checks

The source was compiled from this exported folder with the pinned LLVM-MinGW toolchain. Foundation tests passed. Domain tests checked private party/channel ownership, re-entry, door prerequisites, trigger range, native message bytes, wave activation, duplicate suppression, completion, safe return points and atomic admission.

The capture replay test reproduces **201 door, passage and wave messages across seven dungeon variants**. Its fixture retains native message bytes, actor references and frame numbers, without account or chat data. The Peltrok initialization fixture contains **137 unique native maze-object messages**, observed twice identically in the supplied 07 November capture and matched against that map's object references.

Packet-flow tests cover Peltrok's native NPC entrance and initial maze messages for all seven class families, Berneo dungeon admission/reconnect/exit for all seven classes, and native entrance-ticket checks for the basic catacomb event and belltower. A separate public-world case checks Antcave and Sentryhail entrances, Antcave's internal floor portal, and the absence of private-run membership in those public worlds.

The test helper chooses the canonical first map ID when a scene name occurs more than once in the client map list. For example, `asmara` exists both as map 19002 and a technical map 99998. This was a fixture correction; production character loading already checks the saved map ID.

## Limits

These are automated server and packet tests. They do not establish that every door animation or collision transition looks correct in a running client. This review does not recover an original procedural maze-generation algorithm from the single captured initial layout. Other dungeons and unobserved script paths still need separate evidence and gameplay checks. The previously reported intermittent native-memory issue is not declared resolved by these changes.

The historical official guide distinguishes public normal dungeons, event instances with kill/position triggers, and maze instances with moving walls: https://www.karosgame.com/gameinfo/dungeon.nhn

## Test commands

After building, use authenticated content and NEW isolated database files:

```powershell
.\build\native\KarosDungeonTests.exe 'D:\IsolatedContent' 'D:\Tests\new-domain.sqlite' .\tests\fixtures\dungeon-capture-20261118.json
.\build\native\KarosDungeonDoorReplayTests.exe 'D:\IsolatedContent' 'D:\Tests\new-replay.sqlite' .\tests\fixtures\dungeon-door-replay-20261119.json
.\build\native\KarosDungeonFlowTests.exe 'D:\IsolatedFixture' peltrok
.\build\native\KarosDungeonFlowTests.exe 'D:\AnotherIsolatedFixture' tickets
.\build\native\KarosDungeonFlowTests.exe 'D:\PublicWorldFixture' public
```

Flow fixtures must have a `GM_TEST_FIXTURE` marker, writable clean database copies, compatible authenticated catalogs and their locally generated content key. Test-created accounts are disposable. Never use live player databases as test fixtures.

## Expanded inventory

See [Dungeon coverage](DUNGEON_COVERAGE.md) for all 38 entries. The review stages 24 additional native map definitions and their exact actors/callbacks. 88 event conditions remain unbound; full completion of these scripts is not claimed.
