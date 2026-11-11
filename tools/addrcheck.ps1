# Checks the game addresses the plugin relies on against the installed game, without starting it: every
# Address Library ID the plugin names in src/ resolves in the installed version-1-11-240-0.bin, an ID inside
# the game's code is the start of a function there, and the code the plugin reads or lists by hand looks as it
# was read (the call from Main::Run into the message loop, the engine's pump the frame check looks for, the
# save anchor's test, the HUD menu's update slot). Run it before cutting a release candidate; it prints each
# check and exits 1 when one fails.
param(
	[string]$GamePath = 'F:\SteamLibrary\steamapps\common\Fallout 4'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $GamePath 'Data\F4SE\Plugins\version-1-11-240-0.bin'
$exe = Join-Path $GamePath 'Fallout4.exe'
foreach ($file in $bin, $exe) {
	if (-not (Test-Path $file)) { throw "not found: $file" }
}

# The Address Library (format 0: a count, then id and offset pairs, both 64-bit).
$db = [IO.File]::ReadAllBytes($bin)
$count = [BitConverter]::ToUInt64($db, 0)
$offsets = @{}
for ($i = 0; $i -lt $count; $i++) {
	$at = 8 + $i * 16
	$offsets[[BitConverter]::ToUInt64($db, $at)] = [BitConverter]::ToUInt64($db, $at + 8)
}

# The game's sections and its unwind table (function begin and end, by offset from the image).
$image = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($image, 0x3C)
$sectionCount = [BitConverter]::ToUInt16($image, $pe + 6)
$optionalSize = [BitConverter]::ToUInt16($image, $pe + 20)
$sections = @()
for ($i = 0; $i -lt $sectionCount; $i++) {
	$at = $pe + 24 + $optionalSize + $i * 40
	$sections += [pscustomobject]@{
		Name = [Text.Encoding]::ASCII.GetString($image, $at, 8).TrimEnd([char]0)
		Va   = [BitConverter]::ToUInt32($image, $at + 12)
		Size = [BitConverter]::ToUInt32($image, $at + 8)
		Raw  = [BitConverter]::ToUInt32($image, $at + 20)
	}
}
$imageBase = [BitConverter]::ToUInt64($image, $pe + 24 + 24)

function SectionOf([uint64]$Offset) {
	foreach ($s in $sections) {
		if ($Offset -ge $s.Va -and $Offset -lt $s.Va + $s.Size) { return $s }
	}
	return $null
}

function BytesAt([uint64]$Offset, [int]$Length) {
	$s = SectionOf $Offset
	if (-not $s) { throw ('+{0:X} is in no section' -f $Offset) }
	$from = [int]($Offset - $s.Va + $s.Raw)
	return $image[$from..($from + $Length - 1)]
}

function FileOffset([uint64]$Rva) {
	$s = SectionOf $Rva
	if (-not $s) { throw ('+{0:X} is in no section' -f $Rva) }
	return [int]($Rva - $s.Va + $s.Raw)
}

# The unwind table: each function with a stack frame, [begin, end), sorted by begin.
$pdata = $sections | Where-Object Name -eq '.pdata'
$begins = New-Object 'System.Collections.Generic.List[uint32]'
$ends = New-Object 'System.Collections.Generic.List[uint32]'
for ($at = $pdata.Raw; $at + 12 -le $pdata.Raw + $pdata.Size; $at += 12) {
	$begin = [BitConverter]::ToUInt32($image, $at)
	if ($begin -eq 0) { break }
	$begins.Add($begin)
	$ends.Add([BitConverter]::ToUInt32($image, $at + 4))
}

# The unwind entry at or before an offset (its index), or -1.
function EntryAtOrBefore([uint64]$Offset) {
	$low = 0
	$high = $begins.Count - 1
	$found = -1
	while ($low -le $high) {
		$middle = [int](($low + $high) / 2)
		if ($begins[$middle] -le $Offset) { $found = $middle; $low = $middle + 1 } else { $high = $middle - 1 }
	}
	return $found
}

# A function begins here: an unwind entry begins here, or (a leaf function, which has none) the offset is in no
# other function's code and follows the padding or return that ends the code before it.
function IsFunctionStart([uint64]$Offset) {
	$entry = EntryAtOrBefore $Offset
	if ($entry -ge 0 -and $begins[$entry] -eq $Offset) { return $true }
	if ($entry -ge 0 -and $Offset -lt $ends[$entry]) { return $false }
	$at = FileOffset $Offset
	return $image[$at] -ne 0xCC -and ($image[$at - 1] -eq 0xCC -or $image[$at - 1] -eq 0xC3)
}

# The imports, by the address of their slot: 'USER32.dll!PeekMessageW'.
$imports = @{}
$importDirectory = [BitConverter]::ToUInt32($image, $pe + 24 + 112 + 8)
if ($importDirectory) {
	for ($at = FileOffset $importDirectory; [BitConverter]::ToUInt32($image, $at + 12) -ne 0; $at += 20) {
		$nameAt = FileOffset ([BitConverter]::ToUInt32($image, $at + 12))
		$dll = [Text.Encoding]::ASCII.GetString($image, $nameAt, [Array]::IndexOf($image, [byte]0, $nameAt) - $nameAt)
		$slots = [BitConverter]::ToUInt32($image, $at + 16)
		$names = [BitConverter]::ToUInt32($image, $at)
		if (-not $names) { $names = $slots }
		for ($i = 0; ; $i++) {
			$thunk = [BitConverter]::ToUInt64($image, (FileOffset ($names + $i * 8)))
			if ($thunk -eq 0) { break }
			if ($thunk -band 0x8000000000000000) { continue }
			$functionAt = (FileOffset $thunk) + 2
			$function = [Text.Encoding]::ASCII.GetString($image, $functionAt, [Array]::IndexOf($image, [byte]0, $functionAt) - $functionAt)
			$imports[[uint64]($slots + $i * 8)] = "$dll!$function"
		}
	}
}

# What a call through an import slot (FF 15 disp32) at an offset calls, or $null.
function ImportCalledAt([uint64]$Offset) {
	$call = BytesAt $Offset 6
	if ($call[0] -ne 0xFF -or $call[1] -ne 0x15) { return $null }
	return $imports[[uint64]($Offset + 6 + [BitConverter]::ToInt32($call, 2))]
}
$failed = 0
function Report([bool]$Ok, [string]$What) {
	if ($Ok) { "ok      $What" } else { "FAILED  $What"; $script:failed++ }
}

function Resolve([uint64]$Id) {
	if ($offsets.ContainsKey($Id)) { return $offsets[$Id] }
	return $null
}

function HexMatches([byte[]]$Bytes, [string]$Pattern) {
	$want = $Pattern -split ' '
	for ($i = 0; $i -lt $want.Count; $i++) {
		if ($want[$i] -ne '??' -and $Bytes[$i] -ne [Convert]::ToByte($want[$i], 16)) { return $false }
	}
	return $true
}

# Every ID the plugin names: REL::ID NAME{ n } and REL::ID(n).
$ids = [ordered]@{}
Get-ChildItem (Join-Path $root 'src') -Recurse -Include *.cpp, *.h | ForEach-Object {
	$file = $_
	$text = [IO.File]::ReadAllText($file.FullName)
	foreach ($m in [regex]::Matches($text, 'REL::ID\s*(\w*)\s*[\{\(]\s*(\d+)\s*[\}\)]')) {
		$id = [uint64]$m.Groups[2].Value
		if (-not $ids.Contains($id)) {
			$name = if ($m.Groups[1].Value) { $m.Groups[1].Value } else { 'REL::ID' }
			$ids[$id] = '{0} ({1})' -f $name, $file.Name
		}
	}
}

foreach ($id in $ids.Keys) {
	$offset = Resolve $id
	if ($null -eq $offset) {
		Report $false ('{0} {1}: not in the Address Library' -f $id, $ids[$id])
		continue
	}
	$s = SectionOf $offset
	if (-not $s) {
		Report $false ('{0} {1}: +{2:X} is in no section' -f $id, $ids[$id], $offset)
	} elseif ($s.Name -eq '.text') {
		Report (IsFunctionStart $offset) ('{0} {1}: +{2:X}, the start of a function' -f $id, $ids[$id], $offset)
	} else {
		Report $true ('{0} {1}: +{2:X} in {3}' -f $id, $ids[$id], $offset, $s.Name)
	}
}

# The code read by hand (1.11.240).
$mainRun = Resolve 4484191
$loop = Resolve 2228915
$pump = Resolve 2228998
$anchor = Resolve 2228018
$hudVtable = Resolve 585176

if ($null -ne $mainRun -and $null -ne $loop) {
	$call = BytesAt ($mainRun + 0x2E) 5
	$target = $mainRun + 0x33 + [BitConverter]::ToInt32($call, 1)
	Report ($call[0] -eq 0xE8 -and $target -eq $loop) ('Main::Run+0x2E calls the message loop +{0:X}' -f $loop)
}
if ($null -ne $pump) {
	# PositionPlayerJob::Run: sub rsp,68, then a loop that peeks, translates and dispatches window messages.
	Report (HexMatches (BytesAt $pump 4) '48 83 EC 68') ('PositionPlayerJob::Run +{0:X} begins as read' -f $pump)
	Report ((ImportCalledAt ($pump + 0x85)) -match '!PeekMessage[AW]$') 'PositionPlayerJob::Run peeks for window messages at +0x85'
	Report ((ImportCalledAt ($pump + 0x95)) -eq 'USER32.dll!TranslateMessage') 'PositionPlayerJob::Run translates them at +0x95'
	Report ((ImportCalledAt ($pump + 0xA0)) -match '!DispatchMessage[AW]$') 'PositionPlayerJob::Run dispatches them at +0xA0'
	$entry = EntryAtOrBefore $pump
	Report ($entry -ge 0 -and $begins[$entry] -eq $pump -and $ends[$entry] -eq $pump + 0xEF) 'PositionPlayerJob::Run is 0xEF bytes long in the unwind table'
}
if ($null -ne $anchor) {
	# push rbx; sub rsp,20; mov rbx,rcx; test rcx,rcx; je; call GetWorldSpace; mov rcx,[rbx+B8] (the parent cell).
	Report (HexMatches (BytesAt $anchor 26) '40 53 48 83 EC 20 48 8B D9 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 8B B8 00 00 00') ('the save anchor +{0:X} reads the worldspace, then the cell at +B8' -f $anchor)
}
if ($null -ne $hudVtable) {
	$slot = [BitConverter]::ToUInt64((BytesAt ($hudVtable + 4 * 8) 8), 0) - $imageBase
	Report (IsFunctionStart $slot) ('HUDMenu vtable slot 4 (AdvanceMovie, hooked by Compass) is a function, +{0:X}' -f $slot)
}
# The power armor HUD's vtable (RE::VTABLE::PowerArmorHUDMenu[0], hooked by Compass): its class's primary one (the
# RTTI locator before it names PowerArmorHUDMenu at offset 0), and slot 4 a function.
$armorHudVtable = Resolve 1009779
if ($null -eq $armorHudVtable) {
	Report $false 'PowerArmorHUDMenu vtable 1009779: not in the Address Library'
} else {
	$locator = [BitConverter]::ToUInt64((BytesAt ($armorHudVtable - 8) 8), 0) - $imageBase
	$locatorBytes = BytesAt $locator 24
	$type = [BitConverter]::ToUInt32($locatorBytes, 12)
	$name = [Text.Encoding]::ASCII.GetString((BytesAt ($type + 16) 26))
	Report ([BitConverter]::ToUInt32($locatorBytes, 4) -eq 0 -and $name.StartsWith('.?AVPowerArmorHUDMenu@@')) ('PowerArmorHUDMenu vtable +{0:X} is the primary one of PowerArmorHUDMenu' -f $armorHudVtable)
	$slot = [BitConverter]::ToUInt64((BytesAt ($armorHudVtable + 4 * 8) 8), 0) - $imageBase
	Report (IsFunctionStart $slot) ('PowerArmorHUDMenu vtable slot 4 (AdvanceMovie, hooked by Compass) is a function, +{0:X}' -f $slot)
}

# The texture update the position job waits on (TextureWait.cpp, docs/HANG-20261017.md): its state, the wait's loop on
# it, and the walk that hands a batch over when the streamer's pool runs out.
foreach ($pair in @(@(2698346, 0x32FF859), @(2698349, 0x32FF85C), @(2698350, 0x32FF860), @(2698351, 0x32FF868), @(2698352, 0x32FF870), @(2698354, 0x32FF878),
		@(2698358, 0x32FF8B8), @(2698359, 0x32FF8BC), @(2698368, 0x32FF8E8), @(2703490, 0x34485E0), @(2229488, 0xC54910), @(2275476, 0x17C66D0), @(2275477, 0x17C67C0),
		@(2228917, 0xC339D0), @(2228998, 0xC3B030), @(2698032, 0x32DD0FB))) {
	Report ((Resolve $pair[0]) -eq $pair[1]) ('texture update ID {0} is +{1:X}' -f $pair[0], $pair[1])
}
# Where a rip-relative operand points: the instruction at $Offset is $Length bytes long, its disp32 at $DispAt.
function RipTarget([uint64]$Offset, [int]$DispAt, [int]$Length) {
	return $Offset + $Length + [BitConverter]::ToInt32((BytesAt $Offset $Length), $DispAt)
}
$wait = Resolve 2229488
$submit = Resolve 2275477
$pendingAt = Resolve 2698346
$doneAt = Resolve 2698349
$totalAt = Resolve 2698352
$currentAt = Resolve 2698351
if ($null -ne $wait -and $null -ne $submit) {
	Report (HexMatches (BytesAt $wait 6) '41 56 48 83 EC 30') ('the texture wait +{0:X} begins as read' -f $wait)
	$call = BytesAt ($wait + 0x24B) 5
	Report ($call[0] -eq 0xE8 -and ($wait + 0x250 + [BitConverter]::ToInt32($call, 1)) -eq $submit) 'the texture wait hands its batch over (SubmitBatch) at +0x24B'
	# cmp byte [pending],bpl; je; mov eax,[done]; cmp eax,[total]; jae: the loop, at +0x260.
	Report ((HexMatches (BytesAt ($wait + 0x260) 3) '40 38 2D') -and (RipTarget ($wait + 0x260) 3 7) -eq $pendingAt -and
		(RipTarget ($wait + 0x269) 2 6) -eq $doneAt -and (RipTarget ($wait + 0x26F) 2 6) -eq $totalAt) 'the texture wait loops on pending and done < total at +0x260'
	Report ((BytesAt ($wait + 0x30F) 1)[0] -eq 0xC3) 'the texture wait ends at +0x30F (the length TextureWait.cpp looks for on a stack)'
	# The walk (+C53940, by offset: the plugin never calls it): no request from the pool, so mov rcx,[current]; call
	# SubmitBatch; or [pending],al, and it goes on adding to the same batch next frame.
	$handOver = 0xC53ADD
	$call = BytesAt ($handOver + 7) 5
	Report ((RipTarget $handOver 3 7) -eq $currentAt -and $call[0] -eq 0xE8 -and ($handOver + 12 + [BitConverter]::ToInt32($call, 1)) -eq $submit -and
		(HexMatches (BytesAt ($handOver + 12) 2) '08 05') -and (RipTarget ($handOver + 12) 2 6) -eq $pendingAt) 'the texture walk hands a batch over and sets pending when the pool runs out (+C53ADD)'
}
# The call TextureWait::Install patches: Main::OnIdle tests the position flag (cmp byte [flag],sil; je) and calls
# PositionPlayerJob::Run at +0x37F. The plugin checks the call itself at load; this checks the flag test before it too.
$onIdle = Resolve 2228917
$pump = Resolve 2228998
if ($null -ne $onIdle -and $null -ne $pump) {
	$call = BytesAt ($onIdle + 0x37F) 5
	Report ((HexMatches (BytesAt ($onIdle + 0x372) 3) '40 38 35') -and (RipTarget ($onIdle + 0x372) 3 7) -eq (Resolve 2698032) -and
		(HexMatches (BytesAt ($onIdle + 0x379) 2) '0F 84') -and $call[0] -eq 0xE8 -and ($onIdle + 0x384 + [BitConverter]::ToInt32($call, 1)) -eq $pump) 'Main::OnIdle tests the position flag and calls PositionPlayerJob::Run at +0x37F'
}

# Havok's angle loop HavokGuard::Install puts a detour on (HavokNan.h, docs/HANG-20261011.md): the constraint setup at
# +156F5C0, and its two copies of the exit test (cmpneqss xmm0,xmm5; movd eax,xmm0; bt eax,0; jb back 0x8B to the top).
$constraints = Resolve 2263389
Report ($constraints -eq 0x156F5C0) ('the constraint setup ID 2263389 is +{0:X}' -f $constraints)
if ($null -ne $constraints) {
	foreach ($at in @(0x2369, 0x3038)) {
		Report (HexMatches (BytesAt ($constraints + $at) 19) 'F3 0F C2 C5 04 66 0F 7E C0 0F BA E0 00 0F 82 75 FF FF FF') ('the constraint setup''s angle loop ends at +{0:X} as read' -f $at)
	}
}

# The six calls Face::Install patches (src/game/FaceHeads.h): each calls the prebuilt-head check UsesPreprocessedHead,
# and nothing else in the game calls it. The plugin checks the six itself at load and patches none unless all match.
$usesPreprocessed = Resolve 2209308
if ($null -ne $usesPreprocessed) {
	Report ($usesPreprocessed -eq 0x6DF3A0) ('the prebuilt-head check 2209308 is +{0:X}' -f $usesPreprocessed)
	foreach ($site in @(@(2192002, 0x191), @(2207435, 0x3B), @(2209307, 0xA7), @(2209326, 0x63), @(2209355, 0x62), @(2209355, 0x88))) {
		$fn = Resolve $site[0]
		if ($null -eq $fn) {
			Report $false ('face site {0}: not in the Address Library' -f $site[0])
			continue
		}
		$at = $fn + $site[1]
		$call = BytesAt $at 5
		Report ($call[0] -eq 0xE8 -and ($at + 5 + [BitConverter]::ToInt32($call, 1)) -eq $usesPreprocessed) ('{0}+0x{1:X} (+{2:X}) calls the prebuilt-head check' -f $site[0], $site[1], $at)
	}
}

# The two calls Equipment::InstallFrameUse patches (src/game/FrameUse.h): TESFurniture::Activate's call into the
# player's furniture use, and the player's furniture enter's HUD message. The plugin checks each itself at load.
# And WorkshopSync's: the crafting menus' gather asking Workshop::GetWorkshop (CraftingWorkshop.h), and the light maker
# asking Workshop::IsPowered (InstallLights). And Storage::InstallGather's: the two calls to Workshop::AddOtherContainers
# (the host's supply lines, SupplyLines.h). And WorkshopSync::InstallPicks' fourteen (SharedWorkshop.h): the general and the
# wire pick asking Workshop::IsInteractionBlocked, the store test and the store request asking Workshop::CanStoreRef, the
# scrap button and the scrap request asking Workshop::WorkshopCanScrapRef, the move button and the workshop menu's Accept (the
# grab) asking Workshop::WorkshopCanSelectRef, and the menu's Cancel (the Store key) asking Workshop::CanStorePlacementItem;
# and the picks' own five (the pick watch, and a stand-in passed by): the foreground job calling the general pick, its
# cast's sort, both picks' hits' FindBodyIdRef, and the general filter.
foreach ($site in @(@(2198054, 0x384, 2232956, 'TESFurniture::Activate calls the player''s furniture use'), @(2233039, 0x25F, 2222440, 'the player''s furniture enter shows its message'),
		@(2224943, 0x10D, 2194973, 'the crafting menus'' gather asks Workshop::GetWorkshop'), @(2198256, 0x1A9, 2194916, 'the light maker asks Workshop::IsPowered'),
		@(2195116, 0x178, 2195029, 'Workshop::CollectLinkedWorkshopContainers calls Workshop::AddOtherContainers'), @(2224943, 0x426, 2195029, 'the crafting menus'' gather calls Workshop::AddOtherContainers'),
		@(2195314, 0x4FA, 2195179, 'the general pick asks Workshop::IsInteractionBlocked'), @(2195312, 0x4FA, 2195179, 'the wire pick asks Workshop::IsInteractionBlocked'),
		@(2194963, 0x80, 2195195, 'Workshop::CanStorePlacementItem asks Workshop::CanStoreRef'), @(4477212, 0x8E, 2195195, 'Workshop::RequestStorePlacementItem asks Workshop::CanStoreRef'),
		@(2195192, 0x188, 2194959, 'Workshop::CanScrapPlacementRef asks Workshop::WorkshopCanScrapRef'), @(2194966, 0x85, 2194959, 'Workshop::RequestScrapPlacementItem asks Workshop::WorkshopCanScrapRef'),
		@(2194964, 0x80, 2194958, 'Workshop::CanSelectPlacementRef asks Workshop::WorkshopCanSelectRef'), @(2224981, 0x148D, 2194958, 'the workshop menu''s Accept asks Workshop::WorkshopCanSelectRef'),
		@(2224981, 0xA5B, 2194963, 'the workshop menu''s Cancel asks Workshop::CanStorePlacementItem'),
		@(2194909, 0x46, 2195314, 'Workshop::UpdateForegroundReferenceJob calls the general pick'), @(2195314, 0x469, 2208693, 'the general pick asks TESHavokUtilities::FindBodyIdRef'),
		@(2195312, 0x469, 2208693, 'the wire pick asks TESHavokUtilities::FindBodyIdRef'), @(2195314, 0x3ED, 2256236, 'the general pick sorts its cast''s hits'),
		@(2195314, 0x511, 2195585, 'the general pick calls its filter'))) {
	$fn = Resolve $site[0]
	$target = Resolve $site[2]
	if ($null -eq $fn -or $null -eq $target) {
		Report $false ('frame use site {0}: not in the Address Library' -f $site[0])
		continue
	}
	$at = $fn + $site[1]
	$call = BytesAt $at 5
	Report ($call[0] -eq 0xE8 -and ($at + 5 + [BitConverter]::ToInt32($call, 1)) -eq $target) ('{0}+0x{1:X} (+{2:X}): {3}' -f $site[0], $site[1], $at, $site[3])
}

# Projectile::Launch, which InertShots::Install puts a jump on at its entry (src/game/InertShot.h): the function the
# projectile classes' constructors are called from. It begins by spilling its first argument (mov [rsp+8],rcx: the five
# bytes the jump takes, moved whole into the continuation), then reads the record's flags at +C0 and switches on the
# motion type. The plugin checks the entry itself at load.
$launch = Resolve 2236958
Report ($launch -eq 0xE421E0) ('Projectile::Launch ID 2236958 is +{0:X}' -f $launch)
if ($null -ne $launch) {
	Report (HexMatches (BytesAt $launch 15) '48 89 4C 24 08 55 53 56 57 41 54 41 55 41 57') 'Projectile::Launch begins by spilling rcx, then pushes its registers, as read'
	# mov rax,[rdx+18] (the record); test rax,rax; je; mov ecx,[rax+C0] (its flags); mov eax,ecx; and eax,7F0000 (the motion type).
	Report (HexMatches (BytesAt ($launch + 0x4F) 26) '48 8B 42 18 48 85 C0 0F 84 ?? ?? ?? ?? 8B 88 C0 00 00 00 8B C1 25 00 00 7F 00') 'Projectile::Launch reads the record''s flags and its motion type at +0x4F'
}

# The victim's reaction to an attacker, whose queued call CombatReactions::Install hooks (src/game/CombatReaction.h): the
# task queue's unpack calls it at +0x2DAA (the plugin checks this call itself at load). The reaction reads the victim's
# AI process (mov rax,[rbx+300]) at +0xDA, and hands it unchecked as its this (mov rcx,[rsp+68]) to AIProcess::EnterCombat
# at +0x5D7; its other caller returns at its entry when the victim has none (cmp [rcx+300],r12; je), and calls it at +0x2298.
$react = Resolve 2231180
$enterCombat = Resolve 2231840
Report ($react -eq 0xCD6F70) ('the victim''s reaction to an attacker ID 2231180 is +{0:X}' -f $react)
Report ($enterCombat -eq 0xD00F80) ('AIProcess::EnterCombat ID 2231840 is +{0:X}' -f $enterCombat)
if ($null -ne $react -and $null -ne $enterCombat) {
	Report (HexMatches (BytesAt $react 12) '4C 89 4C 24 20 53 56 57 48 81 EC 90') 'the victim''s reaction begins as read'
	Report (HexMatches (BytesAt ($react + 0xDA) 7) '48 8B 83 00 03 00 00') 'the victim''s reaction reads the victim''s AI process at +0xDA'
	$call = BytesAt ($react + 0x5D7) 5
	Report ((HexMatches (BytesAt ($react + 0x5C9) 5) '48 8B 4C 24 68') -and $call[0] -eq 0xE8 -and ($react + 0x5DC + [BitConverter]::ToInt32($call, 1)) -eq $enterCombat) 'the victim''s reaction hands that process to AIProcess::EnterCombat at +0x5D7'
	foreach ($site in @(@(2229323, 0x2DAA, 'the task queue''s unpack calls the victim''s reaction'), @(2231148, 0x2298, 'the hit''s handling calls the victim''s reaction'))) {
		$fn = Resolve $site[0]
		if ($null -eq $fn) {
			Report $false ('reaction caller {0}: not in the Address Library' -f $site[0])
			continue
		}
		$at = $fn + $site[1]
		$call = BytesAt $at 5
		Report ($call[0] -eq 0xE8 -and ($at + 5 + [BitConverter]::ToInt32($call, 1)) -eq $react) ('{0}+0x{1:X} (+{2:X}): {3}' -f $site[0], $site[1], $at, $site[2])
	}
	$handling = Resolve 2231148
	if ($null -ne $handling) {
		Report ((HexMatches (BytesAt ($handling + 0x22) 3) '4C 8B F9') -and (HexMatches (BytesAt ($handling + 0x2C) 9) '4C 39 A1 00 03 00 00 0F 84')) 'the hit''s handling keeps the victim in r15 and returns at its entry when it has no AI process (+0x2C)'
	}
}

# The inventory list's destructor, whose one call of RemoveAllAgents ExitInventory::Install guards (src/game/ExitInventory.h):
# at +0x7C it loads the inventory interface's singleton (mov rcx,[rip+disp32]) and at +0x87 calls RemoveAllAgents with
# it; RemoveAllAgents takes its lock and then reads the agent array's size through this (+0xC8) at +0x27, where the
# shutdown crashed with the singleton null (the plugin checks the load and the call itself at load).
$listDestructor = Resolve 2194154
$removeAgents = Resolve 2194033
$inventoryInterface = Resolve 4796586
Report ($listDestructor -eq 0x34D730) ('~BGSInventoryList ID 2194154 is +{0:X}' -f $listDestructor)
Report ($removeAgents -eq 0x3486A0) ('BGSInventoryInterface::RemoveAllAgents ID 2194033 is +{0:X}' -f $removeAgents)
Report ($inventoryInterface -eq 0x30EC470) ('the inventory interface''s singleton ID 4796586 is +{0:X}' -f $inventoryInterface)
if ($null -ne $listDestructor -and $null -ne $removeAgents -and $null -ne $inventoryInterface) {
	$load = BytesAt ($listDestructor + 0x7C) 7
	Report ((HexMatches $load[0..2] '48 8B 0D') -and ($listDestructor + 0x83 + [BitConverter]::ToInt32($load, 3)) -eq $inventoryInterface) 'the inventory list''s destructor loads the inventory interface at +0x7C'
	$call = BytesAt ($listDestructor + 0x87) 5
	Report ((HexMatches (BytesAt ($listDestructor + 0x83) 4) '89 44 24 60') -and $call[0] -eq 0xE8 -and ($listDestructor + 0x8C + [BitConverter]::ToInt32($call, 1)) -eq $removeAgents) 'the inventory list''s destructor calls RemoveAllAgents with it at +0x87'
	Report ((HexMatches (BytesAt $removeAgents 16) '48 89 5C 24 18 55 56 41 56 48 83 EC 30 48 8B E9') -and (HexMatches (BytesAt ($removeAgents + 0x20) 9) '48 8D B5 C8 00 00 00 8B 1E')) 'RemoveAllAgents keeps this in rbp and reads its agent array''s size (+0xC8) at +0x27'
}

# The player camera's bleed-out start and its transition, as Downed.cpp reads them (DownRules::Camera): the start
# compares the current state with the bleed-out camera (+138) and the tween (+110); the transition's set-up keeps
# where it comes from at +0x38 and where it goes at +0x40.
$cameraStart = Resolve 2248380
$transitionSetUp = Resolve 2248448
if ($null -ne $cameraStart) {
	Report ((HexMatches (BytesAt ($cameraStart + 0x16) 7) '48 3B 81 38 01 00 00') -and (HexMatches (BytesAt ($cameraStart + 0x30) 7) '48 3B 81 10 01 00 00')) 'the camera''s bleed-out start compares the current state with the bleed-out camera and the tween'
}
if ($null -ne $transitionSetUp) {
	Report ((HexMatches (BytesAt ($transitionSetUp + 0x45) 4) '48 89 43 38') -and (HexMatches (BytesAt ($transitionSetUp + 0x77) 4) '48 89 43 40')) 'the camera transition''s set-up keeps its states at +0x38 (from) and +0x40 (to)'
}
# The player camera's push of a state onto its return stack (Downed.cpp's PushHook, DownRules::Camera::Pushed): the
# return stack's push (+102BCC0) and the ten calls to it Downed::Install hooks, each in the camera's code pushing its
# current state (+28) on its return stack (+68). The plugin checks each call itself at load and hooks those that match.
$cameraPush = Resolve 2248443
Report ($cameraPush -eq 0x102BCC0) ('the camera''s push 2248443 is +{0:X}' -f $cameraPush)
if ($null -ne $cameraPush) {
	foreach ($site in @(@(2248345, 0x44), @(2248360, 0xAD), @(2248365, 0xDD), @(2248370, 0x5E), @(2248373, 0xBC), @(2248380, 0x53), @(2248411, 0x4BF), @(2248411, 0x51E),
			@(2248422, 0x4A), @(2248423, 0x1F))) {
		$fn = Resolve $site[0]
		if ($null -eq $fn) {
			Report $false ('camera push site {0}: not in the Address Library' -f $site[0])
			continue
		}
		$at = $fn + $site[1]
		$call = BytesAt $at 5
		Report ($call[0] -eq 0xE8 -and ($at + 5 + [BitConverter]::ToInt32($call, 1)) -eq $cameraPush) ('{0}+0x{1:X} (+{2:X}) calls the camera''s push' -f $site[0], $site[1], $at)
	}
}
# The camera's SetState (Downed.cpp EndTransition): the current state (+28) told its End (vtable +50), then the new one
# stored there, as a transition's own end does it.
$setState = Resolve 2214742
if ($null -ne $setState) {
	Report ((HexMatches (BytesAt ($setState + 0x10) 4) '48 8B 49 28') -and (HexMatches (BytesAt ($setState + 0x1C) 3) 'FF 50 50') -and (HexMatches (BytesAt ($setState + 0x31) 4) '48 89 5F 28')) 'the camera''s SetState ends the current state (+28, vtable +50) and stores the new one'
}
# The four calls VatsTargets::Install patches (src/game/VatsTargetRules.h): each calls VATS's target check +B16220, which
# begins by resolving the actor's handle (movzx r14d,dl; ...; call GetSmartPointer) and asks IsChild (vtable +3F0) first.
# The plugin checks the four calls itself at load and patches none unless all match.
$vatsCheck = Resolve 2224729
Report ($vatsCheck -eq 0xB16220) ('VATS''s target check 2224729 is +{0:X}' -f $vatsCheck)
if ($null -ne $vatsCheck) {
	Report ((HexMatches (BytesAt $vatsCheck 21) '40 53 56 41 56 48 83 EC 20 44 0F B6 F2 40 32 F6 48 8B D1 48 8D') -and
		(HexMatches (BytesAt ($vatsCheck + 0x2B) 17) '48 8B 03 48 8B CB 48 89 6C 24 40 FF 90 F0 03 00 00')) 'VATS''s target check begins as read: the handle resolved, then IsChild (vtable +3F0)'
	foreach ($site in @(@(2224731, 0x18F, 'the gather of potential targets'), @(2224730, 0x69, 'the reference''s check'), @(2224716, 0xCB, 'VATSMenu::HandleMenuButton'), @(2224725, 0x58, 'the potential target list''s refill'))) {
		$fn = Resolve $site[0]
		if ($null -eq $fn) {
			Report $false ('VATS site {0}: not in the Address Library' -f $site[0])
			continue
		}
		$at = $fn + $site[1]
		$call = BytesAt $at 5
		Report ($call[0] -eq 0xE8 -and ($at + 5 + [BitConverter]::ToInt32($call, 1)) -eq $vatsCheck) ('{0}+0x{1:X} (+{2:X}): {3} calls VATS''s target check' -f $site[0], $site[1], $at, $site[2])
	}
}
# VATS's potential target list (VatsTargets::DescribeTargets, dev only): a BSTArray whose capacity (+8) and size (+10)
# have IDs of their own.
Report ((Resolve 4804507) -eq 0x328FE00 -and (Resolve 4804508) -eq 0x328FE08 -and (Resolve 4804509) -eq 0x328FE10) 'VATS''s potential target list 4804507 is +328FE00, its capacity and size at +8 and +10'

# A notice's sound (Hud.cpp): the menu sound's lookup by name returns at once for a null name, before any hash.
$soundByName = Resolve 2214760
if ($null -ne $soundByName) {
	Report (HexMatches (BytesAt $soundByName 17) '48 83 EC 28 C7 44 24 30 00 00 00 00 48 85 C9 74 71') 'the menu sound''s lookup by name returns for a null name before hashing it'
}

if ($failed) {
	"$failed check(s) failed"
	exit 1
}
'all checks passed'
