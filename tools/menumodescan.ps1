# menumodescan.ps1 [-GamePath <game folder>]: lists, for a human to disassemble, the game code that may stop the world
# while a menu is up (F4Multiplayer-tnl5, phase 0 of the real-time menus): the functions that load the UI singleton and
# use an operand at +0x1E0 (UI::menuMode, the count of menus up that pause the game) or +0x250 (the UI's own timer's
# disable count), and every rip-relative operand that lands in the engine's timer (BSTimer, its +0x48 the disable count
# its delta stops on). It reads the installed Fallout4.exe and the Address Library, as tools/addrcheck.ps1 does; nothing
# is written. Matches are byte patterns, not a disassembly: each is a lead to open in a disassembler, not a finding.
# Attach its output to the issue with the menuclock log (docs/DEV-CHANNEL.md).
param(
	[string]$GamePath = 'F:\SteamLibrary\steamapps\common\Fallout 4'
)

$ErrorActionPreference = 'Stop'

$bin = Join-Path $GamePath 'Data\F4SE\Plugins\version-1-11-240-0.bin'
$exe = Join-Path $GamePath 'Fallout4.exe'
foreach ($file in $bin, $exe) {
	if (-not (Test-Path $file)) { throw "not found: $file" }
}

# The Address Library (format 0: a count, then id and offset pairs, both 64-bit), as addrcheck.ps1 reads it.
$db = [IO.File]::ReadAllBytes($bin)
$count = [BitConverter]::ToUInt64($db, 0)
$offsets = @{}
for ($i = 0; $i -lt $count; $i++) {
	$at = 8 + $i * 16
	$offsets[[BitConverter]::ToUInt64($db, $at)] = [BitConverter]::ToUInt64($db, $at + 8)
}
function Resolve([uint64]$Id) {
	if ($offsets.ContainsKey($Id)) { return $offsets[$Id] }
	throw "ID $Id is not in the Address Library"
}
$uiSlot = Resolve 4796314  # RE::ID::UI::Singleton: the slot holding the UI's address
$timer = Resolve 2703179   # RE::ID::BSTimer::Singleton: the timer itself

# The game's sections and its unwind table, as addrcheck.ps1 reads them.
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
$text = $sections | Where-Object Name -eq '.text'
$pdata = $sections | Where-Object Name -eq '.pdata'
$begins = New-Object 'System.Collections.Generic.List[uint32]'
$ends = New-Object 'System.Collections.Generic.List[uint32]'
for ($at = $pdata.Raw; $at + 12 -le $pdata.Raw + $pdata.Size; $at += 12) {
	$begin = [BitConverter]::ToUInt32($image, $at)
	if ($begin -eq 0) { break }
	$begins.Add($begin)
	$ends.Add([BitConverter]::ToUInt32($image, $at + 4))
}

# The walk over the code, compiled on the fly (Windows PowerShell 5.1: C# 5): a loop over every byte is too slow in
# PowerShell itself.
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;

public static class MenuModeScan
{
	static byte[] image;
	static uint[] begins;
	static uint[] ends;

	// The unwind entry holding an offset: its begin, or 0 (a leaf function, or padding).
	static uint FunctionOf(uint rva)
	{
		int low = 0, high = begins.Length - 1, found = -1;
		while (low <= high) {
			int middle = (low + high) / 2;
			if (begins[middle] <= rva) { found = middle; low = middle + 1; } else { high = middle - 1; }
		}
		return found >= 0 && rva < ends[found] ? begins[found] : 0u;
	}

	static string Bytes(int at, int count)
	{
		var parts = new List<string>();
		for (int i = 0; i < count && at + i < image.Length; i++) { parts.Add(image[at + i].ToString("X2")); }
		return string.Join(" ", parts.ToArray());
	}

	static void Add(SortedDictionary<uint, List<string>> map, uint function, string what)
	{
		List<string> list;
		if (!map.TryGetValue(function, out list)) { list = new List<string>(); map[function] = list; }
		list.Add(what);
	}

	public static string[] Run(byte[] a_image, uint[] a_begins, uint[] a_ends, uint textVa, uint textRaw, uint textSize, ulong imageBase, uint uiSlot, uint timer)
	{
		image = a_image; begins = a_begins; ends = a_ends;
		var loadsUi = new HashSet<uint>();
		var uiOperands = new SortedDictionary<uint, List<string>>();
		var timerOperands = new SortedDictionary<uint, List<string>>();
		int from = (int)textRaw, to = (int)(textRaw + textSize);
		Func<int, uint> rvaOf = delegate(int at) { return (uint)(at - textRaw + textVa); };
		for (int at = from + 2; at + 8 < to; at++) {
			byte modrm = image[at];
			// mov r64,[rip+disp32] (REX.W 8B /r, mod 00 rm 101): the UI's address loaded from its slot.
			if ((modrm & 0xC7) == 0x05 && image[at - 1] == 0x8B && (image[at - 2] == 0x48 || image[at - 2] == 0x4C)) {
				long target = (long)rvaOf(at) + 5 + BitConverter.ToInt32(image, at + 1);
				if (target == uiSlot) { loadsUi.Add(FunctionOf(rvaOf(at))); }
			}
			// Any rip-relative operand (mod 00 rm 101) into the timer, for an immediate of 0, 1, 2 or 4 bytes after it.
			if ((modrm & 0xC7) == 0x05) {
				int disp = BitConverter.ToInt32(image, at + 1);
				foreach (int immediate in new int[] { 0, 1, 2, 4 }) {
					long target = (long)rvaOf(at) + 5 + immediate + disp;
					if (target >= timer && target < timer + 0x50) {
						Add(timerOperands, FunctionOf(rvaOf(at)), string.Format("+{0:X} BSTimer+0x{1:X}{2} (immediate {3}): {4}", rvaOf(at), target - timer,
							target - timer == 0x48 ? " disableCounter" : target - timer == 0x10 ? " delta" : target - timer == 0x14 ? " realTimeDelta" : "", immediate, Bytes(at - 3, 12)));
						break;
					}
				}
			}
			// An operand [reg+disp32] (mod 10) of 0x1E0 or 0x250, with or without a SIB byte.
			uint value = BitConverter.ToUInt32(image, at);
			if (value == 0x1E0 || value == 0x250) {
				byte before = image[at - 1];
				byte twoBefore = image[at - 2];
				bool direct = (before & 0xC0) == 0x80 && (before & 0x07) != 0x04;
				bool withSib = (twoBefore & 0xC0) == 0x80 && (twoBefore & 0x07) == 0x04;
				if (direct || withSib) {
					Add(uiOperands, FunctionOf(rvaOf(at)), string.Format("+{0:X} [reg+0x{1:X}]{2}: {3}", rvaOf(at), value, value == 0x1E0 ? " menuMode" : " uiTimer.disableCounter",
						Bytes(at - (direct ? 4 : 5), 12)));
				}
			}
		}
		var lines = new List<string>();
		lines.Add(string.Format("The UI singleton's slot +{0:X}; the timer +{1:X}; {2} functions load the UI.", uiSlot, timer, loadsUi.Count));
		lines.Add("");
		lines.Add("Functions that load the UI and use +0x1E0 (menuMode) or +0x250 (the UI timer's disable count):");
		foreach (var pair in uiOperands) {
			if (pair.Key == 0 || !loadsUi.Contains(pair.Key)) { continue; }
			lines.Add(string.Format("  function +{0:X} (0x{1:X}):", pair.Key, imageBase + pair.Key));
			foreach (var what in pair.Value) { lines.Add("    " + what); }
		}
		lines.Add("");
		lines.Add("Functions with an operand in the engine's timer (BSTimer):");
		foreach (var pair in timerOperands) {
			lines.Add(pair.Key == 0 ? "  outside any unwind entry:" : string.Format("  function +{0:X} (0x{1:X}):", pair.Key, imageBase + pair.Key));
			foreach (var what in pair.Value) { lines.Add("    " + what); }
		}
		return lines.ToArray();
	}
}
'@

[MenuModeScan]::Run($image, $begins.ToArray(), $ends.ToArray(), $text.Va, $text.Raw, $text.Size, $imageBase, [uint32]$uiSlot, [uint32]$timer)
