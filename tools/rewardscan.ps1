# Writes src/game/RewardLines.h: the dialogue lines whose script gives the player something (a quest's reward handed over
# in conversation), read from the four masters' INFO records and their topic info fragment scripts (TIF_*), read-only from
# the game's own files. Shared quest rewards (common/RewardBook.h, src/game/QuestRewards.cpp) mirror these to the friends'
# games, which never run the line themselves; a quest stage's own script runs in every game the stage is set in, so stage
# rewards need no table.
#
# A line is listed when one of its fragments calls AddItem on the player (Game.GetPlayer() or a PlayerRef property) with an
# item property the INFO's VMAD fills, and none of its fragments takes anything from the player (RemoveItem on the player:
# a purchase, a sale or a swap, which only the player who paid gets). The count is the call's literal (0: a variable, any
# count). A leveled list of caps (its EditorID holds "Caps") is listed as caps (Caps001) with any count; another leveled
# list, and an item of a file not among the four masters, is left out (the rolled item can't be told from others).
#
#   powershell -File tools/rewardscan.ps1 [-Data "<Fallout 4>\Data"] [-Out src/game/RewardLines.h]
param(
	[string]$Data = 'F:\SteamLibrary\steamapps\common\Fallout 4\Data',
	[string]$Out = (Join-Path $PSScriptRoot '..\src\game\RewardLines.h')
)
$ErrorActionPreference = 'Stop'
$questscan = Get-Content -Raw (Join-Path $PSScriptRoot 'questscan.cs')
$rewardscan = @'
public static class RewardScan
{
    class Vm
    {
        public byte[] B; public int P;
        public byte U8() { return B[P++]; }
        public short S16() { var v = System.BitConverter.ToInt16(B, P); P += 2; return v; }
        public ushort U16() { var v = System.BitConverter.ToUInt16(B, P); P += 2; return v; }
        public uint U32() { var v = System.BitConverter.ToUInt32(B, P); P += 4; return v; }
        public string W() { int n = U16(); var s = System.Text.Encoding.ASCII.GetString(B, P, n); P += n; return s; }
        public int Format = 2;
    }

    // a fragment script's property: its single form (0: none) or int literal
    public class Prop { public uint Form; public int? Literal; }

    static void Value(Vm v, int a_type, Prop a_prop, bool a_single)
    {
        switch (a_type)
        {
            case 0: break;
            case 1:
                {
                    uint form; if (v.Format == 1) { form = v.U32(); v.S16(); v.U16(); } else { v.U16(); v.S16(); form = v.U32(); }
                    if (a_single) a_prop.Form = form; break;
                }
            case 2: v.W(); break;
            case 3: { var n = (int)v.U32(); if (a_single) a_prop.Literal = n; break; }
            case 4: v.P += 4; break;
            case 5: v.U8(); break;
            case 6: { int t = v.U8(); Value(v, t, a_prop, a_single); break; }
            case 7: { uint n = v.U32(); for (uint k = 0; k < n; k++) Property(v); break; }
            case 11: case 12: case 13: case 14: case 15: case 16: { uint n = v.U32(); for (uint k = 0; k < n; k++) Value(v, a_type - 10, a_prop, false); break; }
            case 17: { uint n = v.U32(); for (uint k = 0; k < n; k++) { uint m = v.U32(); for (uint j = 0; j < m; j++) Property(v); } break; }
            default: throw new System.Exception("property type " + a_type);
        }
    }

    static System.Tuple<string, Prop> Property(Vm v)
    {
        var name = v.W(); int type = v.U8(); v.U8();
        var p = new Prop(); Value(v, type, p, true);
        return System.Tuple.Create(name, p);
    }

    static System.Tuple<string, System.Collections.Generic.Dictionary<string, Prop>> Script(Vm v)
    {
        var name = v.W();
        var props = new System.Collections.Generic.Dictionary<string, Prop>(System.StringComparer.OrdinalIgnoreCase);
        if (name.Length == 0) return System.Tuple.Create(name, props);
        v.U8(); int n = v.U16();
        for (int k = 0; k < n; k++) { var p = Property(v); props[p.Item1] = p.Item2; }
        return System.Tuple.Create(name, props);
    }

    public class Info { public int File; public uint Id; public int QuestFile = -1; public uint Quest; public string Script = ""; public System.Collections.Generic.Dictionary<string, Prop> Props; }
    public class Form { public string Type = ""; public string Edid = ""; }

    public static readonly System.Collections.Generic.Dictionary<string, Info> infos = new System.Collections.Generic.Dictionary<string, Info>(System.StringComparer.OrdinalIgnoreCase);  // by fragment script name
    public static readonly System.Collections.Generic.List<string> errors = new System.Collections.Generic.List<string>();
    public static readonly System.Collections.Generic.Dictionary<ulong, Form> forms = new System.Collections.Generic.Dictionary<ulong, Form>();  // (file << 32 | id): record type and EditorID

    static int FileIndex(string a_name)
    {
        for (int i = 0; i < GearScan.MASTERS.Length; i++) if (string.Equals(GearScan.MASTERS[i].Esm, a_name, System.StringComparison.OrdinalIgnoreCase)) return i;
        return -1;
    }

    // a form ID of a_self's records as (master index, local ID); master -1: a file not among the four
    static System.Tuple<int, uint> Key(uint a_id, System.Collections.Generic.List<string> a_masters, string a_self)
    {
        int high = (int)(a_id >> 24);
        string file = high < a_masters.Count ? a_masters[high] : a_self;
        return System.Tuple.Create(FileIndex(file), a_id & 0xFFFFFF);
    }

    public static void Esm(string a_path)
    {
        string self = System.IO.Path.GetFileName(a_path);
        var all = System.IO.File.ReadAllBytes(a_path);
        var masters = new System.Collections.Generic.List<string>();
        uint hsize = System.BitConverter.ToUInt32(all, 4);
        for (int s = 24; s < 24 + hsize;)
        {
            string st = System.Text.Encoding.ASCII.GetString(all, s, 4); int ss = System.BitConverter.ToUInt16(all, s + 6 - 2);
            if (st == "MAST") masters.Add(System.Text.Encoding.ASCII.GetString(all, s + 6, ss - 1));
            s += 6 + ss;
        }
        int pos = 24 + (int)hsize;
        System.Tuple<int, uint> topicQuest = null;  // the owner quest of the topic whose INFOs follow
        while (pos < all.Length)
        {
            string type = System.Text.Encoding.ASCII.GetString(all, pos, 4);
            uint size = System.BitConverter.ToUInt32(all, pos + 4);
            if (type == "GRUP") { pos += 24; continue; }
            uint flags = System.BitConverter.ToUInt32(all, pos + 8);
            uint id = System.BitConverter.ToUInt32(all, pos + 12);
            int next = pos + 24 + (int)size;
            var key = Key(id, masters, self);
            bool wanted = type == "INFO" || type == "DIAL" || type == "LVLI" || key.Item1 == FileIndex(self);
            if ((flags & 0x20) != 0 || !wanted || type == "LAND" || type == "NAVM" || type == "REFR" || type == "ACHR" || type == "CELL" || type == "WRLD")
            {
                if (type == "DIAL") topicQuest = null;
                pos = next; continue;
            }
            byte[] body; int bo; int bl;
            if ((flags & 0x40000) != 0)
            {
                int dsize = (int)System.BitConverter.ToUInt32(all, pos + 24);
                body = GearScan.Inflate(all, pos + 28, (int)size - 4, dsize); bo = 0; bl = dsize;
            }
            else { body = all; bo = pos + 24; bl = (int)size; }
            string edid = ""; int vmadAt = -1, vmadSize = 0; uint qnam = 0; int big = -1;
            for (int s = bo; s < bo + bl;)
            {
                string st = System.Text.Encoding.ASCII.GetString(body, s, 4);
                int ss = System.BitConverter.ToUInt16(body, s + 4);
                if (big >= 0) { ss = big; big = -1; }
                if (st == "XXXX") { big = (int)System.BitConverter.ToUInt32(body, s + 6); s += 10; continue; }
                int d = s + 6;
                if (st == "EDID") edid = System.Text.Encoding.ASCII.GetString(body, d, System.Math.Max(0, ss - 1));
                else if (st == "VMAD") { vmadAt = d; vmadSize = ss; }
                else if (st == "QNAM" && type == "DIAL" && ss >= 4) qnam = System.BitConverter.ToUInt32(body, d);
                s = d + ss;
            }
            if (key.Item1 >= 0) forms[(ulong)(uint)key.Item1 << 32 | key.Item2] = new Form { Type = type, Edid = edid };
            if (type == "DIAL") topicQuest = qnam != 0 ? Key(qnam, masters, self) : null;
            if (type == "INFO" && vmadAt >= 0 && key.Item1 >= 0)
            {
                try
                {
                    var v = new Vm { B = body, P = vmadAt };
                    v.S16(); v.Format = v.S16();
                    int own = v.U16();
                    for (int k = 0; k < own; k++) Script(v);
                    if (v.P < vmadAt + vmadSize)
                    {
                        v.U8(); v.U8();
                        var script = Script(v);
                        if (script.Item1.Length > 0)
                        {
                            var name = script.Item1.Contains(":") ? script.Item1.Substring(script.Item1.LastIndexOf(':') + 1) : script.Item1;
                            infos[name] = new Info { File = key.Item1, Id = key.Item2, QuestFile = topicQuest != null ? topicQuest.Item1 : -1, Quest = topicQuest != null ? topicQuest.Item2 : 0, Script = name, Props = script.Item2 };
                        }
                    }
                }
                catch (System.Exception ex) { errors.Add(key.Item1 + ":" + key.Item2.ToString("X6") + " " + ex.Message); }
            }
            pos = next;
        }
    }

    public class Give { public string Item; public string Count; }
    public class Fragments { public System.Collections.Generic.List<Give> Gives = new System.Collections.Generic.List<Give>(); public bool Takes; }

    // What a topic info fragment script's fragments give the player (AddItem) and whether one takes from them (RemoveItem).
    public static Fragments Read(System.Collections.Generic.List<GearScan.Func> a_funcs)
    {
        var f = new Fragments();
        foreach (var fn in a_funcs)
        {
            if (!fn.Name.StartsWith("Fragment_", System.StringComparison.OrdinalIgnoreCase)) continue;
            var src = new System.Collections.Generic.Dictionary<string, string>(System.StringComparer.OrdinalIgnoreCase);
            System.Func<string, string> of = a => src.ContainsKey(a) ? src[a] : a;
            foreach (var ins in fn.Code)
            {
                var op = GearScan.OPS[ins.Op]; var a = ins.Args;
                if (op == "callstatic" && a.Count >= 3 && string.Equals(a[1], "GetPlayer", System.StringComparison.OrdinalIgnoreCase)) src[a[2]] = "PLAYER";
                else if (op == "propget" && a.Count >= 3) src[a[2]] = System.Text.RegularExpressions.Regex.IsMatch(a[0], "^p?PlayerRef$|^Player$", System.Text.RegularExpressions.RegexOptions.IgnoreCase) ? "PLAYER" : a[0];
                else if ((op == "cast" || op == "assign") && a.Count >= 2) src[a[0]] = of(a[1]);
                if (op == "callmethod" && a.Count >= 4)
                {
                    bool player = of(a[1]) == "PLAYER" || System.Text.RegularExpressions.Regex.IsMatch(of(a[1]), "^::p?PlayerRef_var$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                    if (!player) continue;
                    if (string.Equals(a[0], "RemoveItem", System.StringComparison.OrdinalIgnoreCase) || string.Equals(a[0], "RemoveAllItems", System.StringComparison.OrdinalIgnoreCase)) f.Takes = true;
                    else if (string.Equals(a[0], "AddItem", System.StringComparison.OrdinalIgnoreCase) && a.Count >= 5) f.Gives.Add(new Give { Item = of(a[3]), Count = of(a[4]) });
                }
            }
        }
        return f;
    }

    static string PropName(string a_var)
    {
        var m = System.Text.RegularExpressions.Regex.Match(a_var, "^::(\\w+)_var$");
        return m.Success ? m.Groups[1].Value : null;
    }

    public static string Run(string a_data)
    {
        foreach (var m in GearScan.MASTERS) Esm(System.IO.Path.Combine(a_data, m.Esm));
        var rows = new System.Collections.Generic.List<string>();
        var skipped = new System.Collections.Generic.List<string>();
        int lines = 0;
        foreach (var m in GearScan.MASTERS)
        {
            var files = GearScan.Ba2(System.IO.Path.Combine(a_data, m.Ba2), n => n.EndsWith(".pex", System.StringComparison.OrdinalIgnoreCase) && n.IndexOf("TopicInfos", System.StringComparison.OrdinalIgnoreCase) >= 0);
            var names = new System.Collections.Generic.List<string>(files.Keys); names.Sort(System.StringComparer.OrdinalIgnoreCase);
            foreach (var path in names)
            {
                System.Collections.Generic.List<GearScan.Func> funcs;
                try { funcs = GearScan.Pex(files[path]); } catch (System.Exception) { continue; }
                var read = Read(funcs);
                if (read.Gives.Count == 0) continue;
                var script = System.IO.Path.GetFileNameWithoutExtension(path);
                Info info;
                if (!infos.TryGetValue(script, out info)) { skipped.Add(script + ": no INFO names it"); continue; }
                if (read.Takes) { skipped.Add(script + ": takes from the player too (a purchase, a sale or a swap)"); continue; }
                if (info.QuestFile < 0) { skipped.Add(script + ": its topic's quest is in no master scanned"); continue; }
                lines++;
                foreach (var give in read.Gives)
                {
                    var prop = PropName(give.Item);
                    Prop p;
                    if (prop == null || !info.Props.TryGetValue(prop, out p) || p.Form == 0) { skipped.Add(script + ": " + give.Item + " is no form property"); continue; }
                    // the property's form, in the INFO's own file numbering: the four masters' order
                    int itemFile = -1; uint item = 0;
                    var esm = GearScan.MASTERS[info.File].Esm;
                    // VMAD form IDs are in the INFO's file's numbering: its masters, then itself
                    var masters = MastersOf(System.IO.Path.Combine(a_data, esm));
                    var key = Key(p.Form, masters, esm); itemFile = key.Item1; item = key.Item2;
                    if (itemFile < 0) { skipped.Add(script + ": " + prop + " is a form of a file not scanned"); continue; }
                    Form form; forms.TryGetValue((ulong)(uint)itemFile << 32 | item, out form);
                    string count = give.Count;
                    int n; if (!int.TryParse(count, out n)) { Prop c; n = info.Props.TryGetValue(PropName(count) ?? "", out c) && c.Literal.HasValue ? c.Literal.Value : 0; }
                    string note = form != null ? form.Edid : prop;
                    if (form != null && form.Type == "LVLI")
                    {
                        if (form.Edid.IndexOf("Caps", System.StringComparison.OrdinalIgnoreCase) < 0) { skipped.Add(script + ": " + form.Edid + " is a leveled list (not of caps)"); continue; }
                        itemFile = 0; item = 0x00000F; n = 0; note = form.Edid + ", as caps";
                    }
                    rows.Add(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, 0x{3:X6}, {4}, 0x{5:X6}, {6} }},  // {7}: {8}", info.QuestFile, info.Quest, info.File, info.Id, itemFile, item, (n < 0 ? 0 : n), script, note));
                }
            }
        }
        var sb = new System.Text.StringBuilder();
        sb.AppendLine("#pragma once");
        sb.AppendLine();
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine();
        sb.AppendLine("// Generated by tools/rewardscan.ps1 from the four masters' INFO records and their topic info fragment scripts: the dialogue");
        sb.AppendLine("// lines whose script gives the player something and takes nothing (a reward handed over in conversation). Each row is one");
        sb.AppendLine("// item of one line: its quest (the topic's owner), the line (INFO), the item and how many (0: any, a count the script");
        sb.AppendLine("// works out, or a leveled list of caps). Files are StageRules::File indexes. Shared quest rewards (src/game/QuestRewards.cpp)");
        sb.AppendLine("// claim what our player got from such a line and the friends' games add it too. Don't edit by hand: run the tool again.");
        sb.AppendLine("namespace RewardLines");
        sb.AppendLine("{");
        sb.AppendLine("\tstruct Row");
        sb.AppendLine("\t{");
        sb.AppendLine("\t\tstd::uint8_t  questFile;");
        sb.AppendLine("\t\tstd::uint32_t quest;");
        sb.AppendLine("\t\tstd::uint8_t  infoFile;");
        sb.AppendLine("\t\tstd::uint32_t info;");
        sb.AppendLine("\t\tstd::uint8_t  itemFile;");
        sb.AppendLine("\t\tstd::uint32_t item;");
        sb.AppendLine("\t\tstd::uint16_t count;  // 0: any");
        sb.AppendLine("\t};");
        sb.AppendLine();
        sb.AppendLine("\t// " + lines + " lines, " + rows.Count + " items; " + infos.Count + " INFO fragment scripts read, " + errors.Count + " unreadable" +
            (errors.Count > 0 ? " (" + string.Join("; ", errors.GetRange(0, System.Math.Min(5, errors.Count))) + ")" : "") + ". Left out:");
        foreach (var s in skipped) sb.AppendLine("\t// - " + s);
        sb.AppendLine("\tinline constexpr Row ROWS[] = {");
        foreach (var r in rows) sb.AppendLine(r);
        sb.AppendLine("\t};");
        sb.AppendLine("}");
        return sb.ToString();
    }

    static System.Collections.Generic.List<string> MastersOf(string a_path)
    {
        var masters = new System.Collections.Generic.List<string>();
        using (var fs = System.IO.File.OpenRead(a_path))
        {
            var head = new byte[24]; fs.Read(head, 0, 24);
            uint hsize = System.BitConverter.ToUInt32(head, 4);
            var body = new byte[hsize]; fs.Read(body, 0, (int)hsize);
            for (int s = 0; s < hsize;)
            {
                string st = System.Text.Encoding.ASCII.GetString(body, s, 4); int ss = System.BitConverter.ToUInt16(body, s + 4);
                if (st == "MAST") masters.Add(System.Text.Encoding.ASCII.GetString(body, s + 6, ss - 1));
                s += 6 + ss;
            }
        }
        return masters;
    }
}
'@
Add-Type -TypeDefinition ($questscan + "`n" + $rewardscan) -ReferencedAssemblies System.Core -Language CSharp
$text = [RewardScan]::Run($Data)
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), $text.Replace("`r`n", "`n"))
"wrote $Out"
