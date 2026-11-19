# In-game Staff Commands — Complete Reference

This document lists the staff commands found in the local NeoAsmara Core source. It does not cover regular player commands. The source code has not been published yet; confirm that the installed Core was built from this revision before assuming every command is available in a public binary.

## Prefix and syntax

- Staff commands use the `//` prefix, for example `//gmhelp 1`.
- `!` is not a command prefix. Regular game commands such as `/esc` remain separate.
- `<argument>` is required; `[argument]` is optional.
- Replies appear in the in-game chat. Depending on the command, a target character can be identified by name or EID.
- The server checks roles, authentication, ownership and target restrictions.

## Roles and authentication

Permissions are cumulative: Helper, Mod, GM and Admin.

| Role | Command level |
|---|---:|
| Player, Alpha, Beta | 0 |
| Helper (also called Ticket) | 1 |
| Mod | 2 |
| GM | 3 |
| Admin (Dev maps to Admin) | 5 |

Helpers can use level 1 commands without a second login. Mods, GMs and Admins must authenticate their staff session.

- `//staffhelp` — displays staff authentication syntax.
- `//gmstatus` — shows the current role and second-login status.
- `//staffregister <password> <confirmation>` — registers a staff password for the current role. Available to Mod or higher; password must be 12–128 characters with no spaces.
- `//stafflogin <password>` — authenticates the staff session.
- `//stafflogout` — ends staff authentication for the current session.
- `//staffchat <message>` — posts to staff chat; maximum 110 characters.
- `//gmhelp [page]` — shows the staff command index, split across 10 pages.

Changing a role invalidates its staff authentication. Five failed password attempts lock staff login for five minutes.

## Helper or higher

- `//where` — shows the character's map and coordinates.
- `//stats` — shows level, HP, MP, ATK and MATK.
- `//stats 2` — shows attributes, defense, magic defense and Fletta.
- `//inventory` — displays an inventory summary.
- `//cooldowns` — lists active potion cooldowns.
- `//item <item_ID>` — looks up the name and description of a verified item.
- `//finditem <text>` — searches item names and shows up to three matches.
- `//queststatus` — displays the character's saved quest state.
- `//maps [page]` — lists native destinations, eight per page.
- `//online [page]` — lists players on the current channel, five per page.

## Mod or higher

- `//npc` or `//npc activate` — attempts to recover map NPCs and show nearby ones.
- `//npc status` — counts loaded non-combat NPCs on the map.
- `//testdamage [target_level]` — calculates a normal-damage range; level 1–120, default 1.
- `//vanish`, `//vanish on`, `//vanish off` or `//vanish status` — changes or checks character visibility.
- `//teleport <x> <y> <z>` — changes coordinates on the current map. It uses the supplied coordinates and does not calculate ground height; use only known safe coordinates.
- `//map <name_or_ID>` — travels to a verified native destination.
- `//goto <character>` — travels to a loaded character online on the same channel.
- `//heal` — restores the issuing character's HP and MP.
- `//mute <character> <scope> <duration> <reason>` — applies a mute.
- `//unmute <character> <scope>` — removes a mute.
- `//sanctions <character>` — checks active sanctions.

Moderation scopes are `character`, `account` or `hardware`. The aliases `char`/`personagem`, `conta` and `hw` are also accepted. A Mod can mute a character for up to 24 hours. Account mutes require GM or higher; hardware actions require Admin.

## GM or higher

- `//mobinfo <EID>` — shows an entity's name, ID, EID, level, HP, position and target.
- `//findmob <name>` — searches mobs and guards on the current map; enter 2–60 characters.
- `//bosstimers [page]` — lists map bosses and whether they are alive or how long until they respawn.
- `//god`, `//god on`, `//god off` or `//god status` — changes or checks God mode.
- `//movespeed <1..3>` — sets the movement-speed multiplier.
- `//attackspeed <1..5>` — sets the attack-speed multiplier.
- `//spawn <mob_ID> [quantity]` — summons 1–10 entities of the specified ID near the character.
- `//respawn` — schedules mob respawns on the current map. It does **not** respawn the player.
- `//announce <message>` — sends an announcement to active channels; maximum 110 characters.
- `//kick <character> <reason>` — disconnects the target's sessions. A character with an equal or higher role cannot be kicked.
- `//ban <character> <scope> <duration> <reason>` — applies a ban. Accepted durations: `30m`, `2h`, `7d`, `perm`, `permanent` or `permanente`; maximum ten years. Character or account bans require GM; hardware bans require Admin.
- `//unban <character> <scope>` — revokes a ban, subject to the same scope permissions.

## Admin or higher

- `//profession <profession_ID>` — changes profession if class family, level and hierarchy are valid.
- `//fletta` — checks the current Fletta amount.
- `//fletta <0..1000000>` — sets the character's Fletta.
- `//give <item_ID> [quantity]` — grants an item to the issuing character. Quantity is 1–10000; equipment is limited to one instance per grant.
- `//carats` — checks the Carats balance.
- `//carats <delta>` — adds or removes Carats; a negative value removes them.
- `//setlevel <1..120>` — sets the character's level.
- `//setexp <total_XP>` — sets total experience.
- `//starter` — grants the test starter pack once per character.
- `//forcesave` — saves the character and requests saves from other open sessions on the account.
- `//passwall [distance]` — moves the character forward; default 150, range 1–5000. `//noclip` is an alias, but this does not enable flight or guarantee passage through collision.
- `//debug`, `//debug on`, `//debug off` or `//debug status` — changes or checks debug mode.
- `//devinspect` — shows mob/NPC counts and the map-loading state.
- `//devinspect <EID>` — shows the entity's state, HP and target.
- `//addcash <character> <amount> <reason>` — credits Cash to the account. Amount is 1–1,000,000,000; reason is required and limited to 1–200 characters.
- `//cashbalance <character>` — checks the account's Cash balance.
- `//setrole <character> <Alpha|Beta|Helper|Mod|GM|Admin|Player>` — grants or removes a role. `Ticket` maps to Helper; `Dev` maps to Admin.
- `//restart [seconds]`, `//reset [seconds]` or `//shutdown [seconds]` — schedules the action; default 60 seconds, range 0–3600. The Core requests saves before execution.
- `//cancel` — cancels an active restart, reset or shutdown countdown.

## Authenticated server owner

These commands also require the Admin role, staff login and an account configured as the server's title owner:

- `//titlecatalog` — lists title IDs, names and rarity.
- `//titlegrant <character> <ID|role> [reason]` — grants a title. `role` selects the title associated with the target's current role.
- `//titlerevoke <character> <ID|role> [reason]` — revokes a title.
- `//itemtrace <UID> [page]` — traces the origin, creator, recipient, current location and history of an item generated through the GM Tool.

Role title IDs: Alpha `1101`, Beta `1102`, Helper `2001`, Mod `2002`, GM `2003`, Admin `2004`.

## Aliases

- `//modhelp`, `//adminhelp`, `//devhelp` → `//gmhelp`.
- `//modstatus`, `//adminstatus`, `//devstatus` → `//gmstatus`.
- `//staff` and `//sc` → `//staffchat`.
- `//carat` → `//carats`.
- `//speedattack` and `//aspeed` → `//attackspeed`.
- `//noclip` → `//passwall`.
- `//rastrearitem` → `//itemtrace`.
- `//adminauth`, `//devauth`, `//gmauth` and `//modauth` were removed; use `//staffregister` and `//stafflogin`.
