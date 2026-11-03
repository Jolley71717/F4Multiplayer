using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

// Read-only scan of the game's files: which quests are which type (ESM QUST DNAM), and which quest
// fragment functions call inventory/equipment functions (pex in a GNRL BA2).
public static class GearScan
{
    public static byte[] Inflate(byte[] data, int offset, int length, int size)
    {
        using (var ms = new MemoryStream(data, offset + 2, length - 2))
        using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
        {
            var outb = new byte[size];
            int read = 0;
            while (read < size)
            {
                int n = ds.Read(outb, read, size - read);
                if (n <= 0) break;
                read += n;
            }
            return outb;
        }
    }

    // ---------------- ESM ----------------
    public class Quest { public uint Id; public string Edid = ""; public int Type = -1; public int Flags; public string Full = ""; public int Stages; public int Objectives; public List<int> Startup = new List<int>(); }

    public static List<Quest> Quests(string path, int fileIndex)
    {
        var all = File.ReadAllBytes(path);
        var list = new List<Quest>();
        // TES4 header record
        int pos = 0;
        uint hsize = BitConverter.ToUInt32(all, 4);
        pos = 24 + (int)hsize;
        while (pos < all.Length)
        {
            string type = Encoding.ASCII.GetString(all, pos, 4);
            uint gsize = BitConverter.ToUInt32(all, pos + 4);
            string label = Encoding.ASCII.GetString(all, pos + 8, 4);
            if (type == "GRUP" && label == "QUST")
            {
                int end = pos + (int)gsize;
                int p = pos + 24;
                while (p < end)
                {
                    string rt = Encoding.ASCII.GetString(all, p, 4);
                    uint rsize = BitConverter.ToUInt32(all, p + 4);
                    if (rt == "GRUP") { p += 24; continue; } // descend into children groups
                    uint flags = BitConverter.ToUInt32(all, p + 8);
                    uint id = BitConverter.ToUInt32(all, p + 12);
                    byte[] body; int bo; int bl;
                    if ((flags & 0x40000) != 0)
                    {
                        int dsize = (int)BitConverter.ToUInt32(all, p + 24);
                        body = Inflate(all, p + 28, (int)rsize - 4, dsize); bo = 0; bl = dsize;
                    }
                    else { body = all; bo = p + 24; bl = (int)rsize; }
                    if (rt == "QUST")
                    {
                        var q = new Quest();
                        q.Id = (id & 0x00FFFFFF) | ((uint)fileIndex << 24);
                        int s = bo; int se = bo + bl; int big = -1;
                        while (s < se)
                        {
                            string st = Encoding.ASCII.GetString(body, s, 4);
                            int ss = BitConverter.ToUInt16(body, s + 4);
                            if (big >= 0) { ss = big; big = -1; }
                            if (st == "XXXX") { big = (int)BitConverter.ToUInt32(body, s + 6); s += 6 + 4; continue; }
                            int d = s + 6;
                            if (st == "EDID") q.Edid = Encoding.ASCII.GetString(body, d, Math.Max(0, ss - 1));
                            if (st == "DNAM" && ss >= 12) { q.Flags = BitConverter.ToUInt16(body, d); q.Type = body[d + 8]; }
                            if (st == "INDX") { q.Stages++; if (ss >= 3 && (body[d + 2] & 0x02) != 0) q.Startup.Add(BitConverter.ToUInt16(body, d)); }
                            if (st == "QOBJ") q.Objectives++;
                            s = d + ss;
                        }
                        list.Add(q);
                    }
                    p += 24 + (int)rsize;
                }
                break;
            }
            pos += (int)gsize;
        }
        return list;
    }

    // ---------------- BA2 ----------------
    public static Dictionary<string, byte[]> Ba2(string path, Func<string, bool> want)
    {
        var result = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        using (var fs = File.OpenRead(path))
        using (var br = new BinaryReader(fs))
        {
            br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt32();
            uint count = br.ReadUInt32();
            ulong nameTable = br.ReadUInt64();
            var offs = new ulong[count]; var packed = new uint[count]; var unpacked = new uint[count];
            for (int i = 0; i < count; i++)
            {
                br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt32();
                offs[i] = br.ReadUInt64(); packed[i] = br.ReadUInt32(); unpacked[i] = br.ReadUInt32(); br.ReadUInt32();
            }
            fs.Position = (long)nameTable;
            var names = new string[count];
            for (int i = 0; i < count; i++) { int len = br.ReadUInt16(); names[i] = Encoding.ASCII.GetString(br.ReadBytes(len)); }
            for (int i = 0; i < count; i++)
            {
                if (!want(names[i])) continue;
                fs.Position = (long)offs[i];
                if (packed[i] == 0) { result[names[i]] = br.ReadBytes((int)unpacked[i]); continue; }
                var raw = br.ReadBytes((int)packed[i]);
                result[names[i]] = Inflate(raw, 0, raw.Length, (int)unpacked[i]);
            }
        }
        return result;
    }

    // ---------------- PEX ----------------
    public class Instr { public int Op; public List<string> Args = new List<string>(); }
    public class Func
    {
        public string Obj; public string State; public string Name; public List<Instr> Code = new List<Instr>();
        // each parameter's and local's type, by name, so a call on one can be followed into its script
        public Dictionary<string, string> Types = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public List<string> Params = new List<string>();  // in order
    }
    // a script object: its parent, and its properties' and variables' types by name
    public class Obj { public string Name; public string Parent = ""; public Dictionary<string, string> Types = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase); public Dictionary<string, string> Defaults = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase); }

    class R
    {
        public byte[] B; public int P; public string[] S;
        public byte U8() { return B[P++]; }
        public ushort U16() { var v = BitConverter.ToUInt16(B, P); P += 2; return v; }
        public uint U32() { var v = BitConverter.ToUInt32(B, P); P += 4; return v; }
        public ulong U64() { var v = BitConverter.ToUInt64(B, P); P += 8; return v; }
        public string W() { int n = U16(); var s = Encoding.ASCII.GetString(B, P, n); P += n; return s; }
        public string Str() { return S[U16()]; }
        public string Var()
        {
            byte t = U8();
            switch (t)
            {
                case 0: return "None";
                case 1: return S[U16()];
                case 2: return "\"" + S[U16()] + "\"";
                case 3: return ((int)U32()).ToString();
                case 4: { var f = BitConverter.ToSingle(B, P); P += 4; return f.ToString(); }
                case 5: return U8() != 0 ? "True" : "False";
                default: throw new Exception("var type " + t);
            }
        }
    }

    static readonly int[] ARGS = { 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, 3, 2, 3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4, 3, 1, 3, 3, 5, 5, 3, 3, 1, 3, 1 };
    public static readonly string[] OPS = { "nop", "iadd", "fadd", "isub", "fsub", "imul", "fmul", "idiv", "fdiv", "imod", "not", "ineg", "fneg", "assign", "cast", "cmp_eq", "cmp_lt", "cmp_le", "cmp_gt", "cmp_ge", "jmp", "jmpt", "jmpf", "callmethod", "callparent", "callstatic", "return", "strcat", "propget", "propset", "array_create", "array_length", "array_getelement", "array_setelement", "array_findelement", "array_rfindelement", "is", "struct_create", "struct_get", "struct_set", "array_findstruct", "array_rfindstruct", "array_add", "array_insert", "array_removelast", "array_remove", "array_clear" };

    static Func ReadFunc(R r, string obj, string state, string name)
    {
        var f = new Func { Obj = obj, State = state, Name = name };
        r.U16(); r.U16(); r.U32(); r.U8();
        int pc = r.U16(); for (int i = 0; i < pc; i++) { var n = r.Str(); f.Types[n] = r.Str(); f.Params.Add(n); }
        int lc = r.U16(); for (int i = 0; i < lc; i++) { var n = r.Str(); f.Types[n] = r.Str(); }
        int ic = r.U16();
        for (int i = 0; i < ic; i++)
        {
            var ins = new Instr { Op = r.U8() };
            int n = ARGS[ins.Op];
            for (int a = 0; a < n; a++) ins.Args.Add(r.Var());
            if (ins.Op == 23 || ins.Op == 24 || ins.Op == 25)
            {
                var c = r.Var(); int cnt = int.Parse(c);
                for (int a = 0; a < cnt; a++) ins.Args.Add(r.Var());
            }
            f.Code.Add(ins);
        }
        return f;
    }

    public static List<Func> Pex(byte[] b) { List<Obj> objs; return Pex(b, out objs); }

    public static List<Func> Pex(byte[] b, out List<Obj> a_objs)
    {
        a_objs = new List<Obj>();
        var r = new R { B = b };
        if (r.U32() != 0xFA57C0DE) throw new Exception("magic");
        r.U8(); r.U8(); r.U16(); r.U64(); r.W(); r.W(); r.W();
        int sc = r.U16(); r.S = new string[sc]; for (int i = 0; i < sc; i++) r.S[i] = r.W();
        if (r.U8() != 0)
        {
            r.U64();
            int fc = r.U16(); for (int i = 0; i < fc; i++) { r.U16(); r.U16(); r.U16(); r.U8(); int l = r.U16(); r.P += 2 * l; }
            int pg = r.U16(); for (int i = 0; i < pg; i++) { r.U16(); r.U16(); r.U16(); r.U32(); int l = r.U16(); r.P += 2 * l; }
            int so = r.U16(); for (int i = 0; i < so; i++) { r.U16(); r.U16(); int l = r.U16(); r.P += 2 * l; }
        }
        int uf = r.U16(); for (int i = 0; i < uf; i++) { r.U16(); r.U8(); }
        var funcs = new List<Func>();
        int oc = r.U16();
        for (int o = 0; o < oc; o++)
        {
            string oname = r.Str(); r.U32();
            var obj = new Obj { Name = oname, Parent = r.Str() }; a_objs.Add(obj);
            r.U16(); r.U8(); r.U32(); r.U16();
            int structs = r.U16();
            for (int s = 0; s < structs; s++) { r.U16(); int m = r.U16(); for (int k = 0; k < m; k++) { r.U16(); r.U16(); r.U32(); r.Var(); r.U8(); r.U16(); } }
            int vc = r.U16(); for (int v = 0; v < vc; v++) { var vn = r.Str(); obj.Types[vn] = r.Str(); r.U32(); obj.Defaults[vn] = r.Var(); r.U8(); }
            int pcnt = r.U16();
            for (int p = 0; p < pcnt; p++)
            {
                string pn = r.Str(); obj.Types[pn] = r.Str(); r.U16(); r.U32(); byte fl = r.U8();
                if ((fl & 4) != 0) r.U16();
                else { if ((fl & 1) != 0) ReadFunc(r, oname, "", pn + ".get"); if ((fl & 2) != 0) ReadFunc(r, oname, "", pn + ".set"); }
            }
            int stc = r.U16();
            for (int s = 0; s < stc; s++)
            {
                string sn = r.Str(); int fc = r.U16();
                for (int k = 0; k < fc; k++) { string fn = r.Str(); funcs.Add(ReadFunc(r, oname, sn, fn)); }
            }
        }
        return funcs;
    }

    public static string Text(Instr i) { return OPS[i.Op] + " " + string.Join(", ", i.Args.ToArray()); }
}

// The stages of the game's quests whose fragment takes from the player, unequips them or empties their inventory
// (RemoveItem, RemoveAllItems, SellItem, UnequipItem, UnequipItemSlot, UnequipAll on Game.GetPlayer() or a PlayerRef),
// each with whether a session shares the quest (QuestFilter: types 1-5, 7, 8+; the factions' finales; the dialogue
// quests, no type and no objectives, named Dialogue<Place>). What src/game/StageRules.h's kPays was checked against.
public static class QuestScan
{
    static readonly HashSet<string> TAKES = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "RemoveAllItems", "UnequipAll", "RemoveItem", "UnequipItem", "UnequipItemSlot", "SellItem", "RemoveItemByComponent" };
    static readonly uint[] FINALES = { 0x0A274B, 0x0B9F9D, 0x173ED9 };

    static bool IsDialogueName(string a_edid)
    {
        var rest = a_edid;
        if (rest.StartsWith("DLC"))
        {
            int digits = 0;
            while (3 + digits < rest.Length && char.IsDigit(rest[3 + digits])) digits++;
            if (digits > 0) { rest = rest.Substring(3 + digits); if (rest.StartsWith("_")) rest = rest.Substring(1); }
        }
        return rest.StartsWith("Dialogue");
    }

    public static string Shared(GearScan.Quest q, int a_file)
    {
        if (q.Type > 0 && q.Type != 6) return "shared";
        if (q.Type == 0 && a_file == 0 && FINALES.Contains(q.Id & 0xFFFFFF)) return "shared (finale)";
        if (q.Type == 0 && q.Objectives == 0 && IsDialogueName(q.Edid)) return "shared (dialogue)";
        return "not shared";
    }

    public static string Run(string a_data)
    {
        var sb = new StringBuilder();
        var files = new[] { new { Esm = "Fallout4.esm", Ba2 = "Fallout4 - Misc.ba2" }, new { Esm = "DLCCoast.esm", Ba2 = "DLCCoast - Main.ba2" } };
        for (int file = 0; file < files.Length; file++)
        {
            var quests = new Dictionary<uint, GearScan.Quest>();
            foreach (var q in GearScan.Quests(Path.Combine(a_data, files[file].Esm), file)) quests[q.Id & 0xFFFFFF] = q;
            var scripts = GearScan.Ba2(Path.Combine(a_data, files[file].Ba2), n => n.IndexOf(@"\fragments\quests\", StringComparison.OrdinalIgnoreCase) >= 0 && n.EndsWith(".pex", StringComparison.OrdinalIgnoreCase));
            sb.AppendLine(string.Format("# {0}: {1} quests, {2} quest fragment scripts", files[file].Esm, quests.Count, scripts.Count));
            var lines = new SortedSet<string>();
            foreach (var kv in scripts)
            {
                var name = Path.GetFileNameWithoutExtension(kv.Key);
                uint id;
                if (!uint.TryParse(name.Substring(name.LastIndexOf('_') + 1), System.Globalization.NumberStyles.HexNumber, null, out id)) continue;
                GearScan.Quest quest;
                if (!quests.TryGetValue(id & 0xFFFFFF, out quest)) continue;
                List<GearScan.Func> funcs;
                try { funcs = GearScan.Pex(kv.Value); }
                catch (Exception ex) { sb.AppendLine("  could not read " + kv.Key + ": " + ex.Message); continue; }
                foreach (var f in funcs)
                {
                    if (!f.Name.StartsWith("Fragment_Stage_", StringComparison.OrdinalIgnoreCase)) continue;
                    var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    var hits = new List<string>();
                    for (int n = 0; n < f.Code.Count; n++)
                    {
                        var i = f.Code[n];
                        // Which variables hold the player now: a temp written with anything else no longer does.
                        if (i.Op == 25)
                        {
                            if (i.Args[0].Equals("game", StringComparison.OrdinalIgnoreCase) && i.Args[1].Equals("GetPlayer", StringComparison.OrdinalIgnoreCase)) players.Add(i.Args[2]);
                            else players.Remove(i.Args[2]);
                        }
                        if (i.Op == 13 || i.Op == 14)
                        {
                            if (players.Contains(i.Args[1]) || i.Args[1].IndexOf("PlayerRef", StringComparison.OrdinalIgnoreCase) >= 0) players.Add(i.Args[0]);
                            else players.Remove(i.Args[0]);
                        }
                        if (i.Op != 23 || !TAKES.Contains(i.Args[0]))
                        {
                            if (i.Op == 23) players.Remove(i.Args[2]);
                            continue;
                        }
                        var self = i.Args[1];
                        if (!players.Contains(self) && self.IndexOf("PlayerRef", StringComparison.OrdinalIgnoreCase) < 0) continue;
                        var item = i.Args.Count > 3 ? i.Args[3] : "";
                        string source = item;
                        for (int k = n - 1; k >= 0; k--)
                        {
                            var x = f.Code[k];
                            if ((x.Op == 13 || x.Op == 14) && x.Args[0] == item) { source = x.Args[1]; break; }
                        }
                        hits.Add(i.Args[0] + "(" + (i.Args[0].Equals("RemoveAllItems", StringComparison.OrdinalIgnoreCase) || i.Args[0].Equals("UnequipAll", StringComparison.OrdinalIgnoreCase) ? "" : source.Replace("::", "").Replace("_var", "")) +
                            (i.Args.Count > 4 && i.Args[0].Equals("RemoveItem", StringComparison.OrdinalIgnoreCase) ? " x" + i.Args[4] : "") + ")");
                    }
                    if (hits.Count == 0) continue;
                    int stage;
                    int.TryParse(f.Name.Substring("Fragment_Stage_".Length, 4), out stage);
                    lines.Add(string.Format("{0:X6} {1,-36} type={2,-2} {3,-18} stage {4,5}: {5}", quest.Id & 0xFFFFFF, quest.Edid, quest.Type, Shared(quest, file), stage, string.Join(" ", hits.Distinct().ToArray())));
                }
            }
            foreach (var line in lines) sb.AppendLine(line);
        }
        return sb.ToString();
    }
}

// The shared quests' stages whose own fragment sets a stage of another quest (SetStage on a quest property other than
// its own quest), following the stages of its own quest it sets: what src/game/StageRules.h's SETS_ANOTHER holds. A
// property is matched to a quest by its name (MQ102_var, pMQ103_var); one that matches no quest is listed with (?), and
// kept in the table, since it may be a shared quest by another name. One line per stage: file, quest, stage, quest
// name, and what it sets.

public static class CrossScan
{
    public static string Run(string data)
    {
        var sb = new StringBuilder();
        var files = new[] { new { Esm = "Fallout4.esm", Ba2 = "Fallout4 - Misc.ba2" }, new { Esm = "DLCCoast.esm", Ba2 = "DLCCoast - Main.ba2" } };
        // every quest by editor ID, of both files (a DLC stage can set a base game quest's)
        var byEdid = new Dictionary<string, Tuple<GearScan.Quest, int>>(StringComparer.OrdinalIgnoreCase);
        var perFile = new List<Dictionary<uint, GearScan.Quest>>();
        for (int f = 0; f < files.Length; f++)
        {
            var d = new Dictionary<uint, GearScan.Quest>();
            foreach (var q in GearScan.Quests(Path.Combine(data, files[f].Esm), f)) { d[q.Id & 0xFFFFFF] = q; if (!byEdid.ContainsKey(q.Edid)) byEdid[q.Edid] = Tuple.Create(q, f); }
            perFile.Add(d);
        }
        for (int file = 0; file < files.Length; file++)
        {
            var quests = perFile[file];
            var scripts = GearScan.Ba2(Path.Combine(data, files[file].Ba2), n => n.IndexOf(@"\fragments\quests\", StringComparison.OrdinalIgnoreCase) >= 0 && n.EndsWith(".pex", StringComparison.OrdinalIgnoreCase));
            var lines = new SortedSet<string>();
            foreach (var kv in scripts)
            {
                var name = Path.GetFileNameWithoutExtension(kv.Key);
                uint id;
                if (!uint.TryParse(name.Substring(name.LastIndexOf('_') + 1), System.Globalization.NumberStyles.HexNumber, null, out id)) continue;
                GearScan.Quest quest;
                if (!quests.TryGetValue(id & 0xFFFFFF, out quest) || QuestScan.Shared(quest, file) == "not shared") continue;
                List<GearScan.Func> funcs;
                try { funcs = GearScan.Pex(kv.Value); } catch { continue; }
                // per stage: the own stages it sets, the other quests' stages it sets
                var self = new Dictionary<int, List<int>>();
                var other = new Dictionary<int, List<string>>();
                foreach (var f in funcs)
                {
                    if (!f.Name.StartsWith("Fragment_Stage_", StringComparison.OrdinalIgnoreCase)) continue;
                    int stage; if (!int.TryParse(f.Name.Substring("Fragment_Stage_".Length, 4), out stage)) continue;
                    if (!self.ContainsKey(stage)) { self[stage] = new List<int>(); other[stage] = new List<string>(); }
                    var kmy = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "self", "kmyQuest", "__temp" };
                    foreach (var i in f.Code)
                    {
                        if (i.Op != 23 || !i.Args[0].Equals("SetStage", StringComparison.OrdinalIgnoreCase)) continue;
                        var obj = i.Args[1];
                        var val = i.Args.Count > 3 ? i.Args[3] : "?";
                        if (kmy.Contains(obj)) { int s; if (int.TryParse(val, out s)) self[stage].Add(s); continue; }
                        var n = obj.Replace("::", ""); if (n.EndsWith("_var")) n = n.Substring(0, n.Length - 4);
                        Tuple<GearScan.Quest, int> t;
                        if (!byEdid.TryGetValue(n, out t) && n.StartsWith("p")) byEdid.TryGetValue(n.Substring(1), out t);
                        if (t == null) { other[stage].Add(n + "(?) " + val); continue; }
                        if (QuestScan.Shared(t.Item1, t.Item2) != "not shared") other[stage].Add(t.Item1.Edid + " " + val);
                    }
                }
                foreach (var st in self.Keys)
                {
                    // follow its own quest's stages it sets
                    var seen = new HashSet<int>(); var todo = new Stack<int>(); todo.Push(st); var hits = new List<string>();
                    while (todo.Count > 0) { var s = todo.Pop(); if (!seen.Add(s) || !self.ContainsKey(s)) continue; hits.AddRange(other[s].Select(h => s == st ? h : h + " (via " + s + ")")); foreach (var n in self[s]) todo.Push(n); }
                    if (hits.Count == 0) continue;
                    lines.Add(string.Format("{0} {1:X6} {2,5} {3,-34} -> {4}", file, quest.Id & 0xFFFFFF, st, quest.Edid, string.Join("; ", hits.Distinct().ToArray())));
                }
            }
            foreach (var l in lines) sb.AppendLine(l);
        }
        return sb.ToString();
    }
}

// The shared quests' stages that move the player (src/game/StageRules.h's kMoves): a stage whose fragment, or a script
// function it calls (its quest's own script through kmyQuest, or any script a property or local is typed with, a few
// calls deep), moves the player (MoveTo on the player), fades the screen out (Game.FadeOutGame(true, ...)), takes their
// movement away (DisablePlayerControls with movement, EnableMovement(false)) or makes them use a door (Activate on a
// *Door* with the player); or that sets a stage of its own quest or of ANY other quest (shared or not, matched by the
// property's name) or starts one (Start(), or a SetStage on it, running its start-up stages too), that does. MQ202 300,
// Dangerous Minds' lounger, does nothing to the player itself: it sets MQ203 10 (Kellogg's memories, not shared), whose
// fragment calls MQ203Script.StartQuest, which fades out and moves the player into the memory. One line per stage that
// moves: file, quest, stage, quest name, and the first way found that it moves (a chain of what it sets and calls).
// What it does not see: a scene a stage starts, a timer or event a script waits for, a call through a variable whose
// type has no script (an ObjectReference property's attached script), a quest matched by no property name.
public static class MoveScan
{
    class Script { public GearScan.Obj Obj; public Dictionary<string, GearScan.Func> Funcs = new Dictionary<string, GearScan.Func>(StringComparer.OrdinalIgnoreCase); }

    static readonly Dictionary<string, Script> scripts = new Dictionary<string, Script>(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, Tuple<GearScan.Quest, int>> byEdid = new Dictionary<string, Tuple<GearScan.Quest, int>>(StringComparer.OrdinalIgnoreCase);
    // per quest (file, id): its stage fragments, by stage
    static readonly Dictionary<Tuple<int, uint>, Dictionary<int, List<GearScan.Func>>> fragments = new Dictionary<Tuple<int, uint>, Dictionary<int, List<GearScan.Func>>>();
    static readonly Dictionary<string, string> funcMoves = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, string> stageMoves = new Dictionary<string, string>();

    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }
    static bool Own(string a_var) { return Is(a_var, "self") || Is(a_var, "kmyQuest") || Is(a_var, "__temp"); }

    static GearScan.Func Find(string a_type, string a_func, out string a_owner)
    {
        a_owner = null;
        for (int depth = 0; !string.IsNullOrEmpty(a_type) && depth < 16; depth++)
        {
            Script s;
            if (!scripts.TryGetValue(a_type, out s)) return null;
            GearScan.Func f;
            if (s.Funcs.TryGetValue(a_func, out f)) { a_owner = a_type; return f; }
            a_type = s.Obj.Parent;
        }
        return null;
    }

    static string TypeOf(GearScan.Func f, string a_var)
    {
        if (Is(a_var, "self")) return f.Obj;
        string t;
        if (f.Types.TryGetValue(a_var, out t)) return t;
        Script s;
        for (var o = f.Obj; !string.IsNullOrEmpty(o) && scripts.TryGetValue(o, out s); o = s.Obj.Parent)
            if (s.Obj.Types.TryGetValue(a_var, out t) || s.Obj.Types.TryGetValue(Bare(a_var), out t)) return t;
        return null;
    }

    static Tuple<GearScan.Quest, int> QuestOf(string a_var)
    {
        var n = Bare(a_var);
        Tuple<GearScan.Quest, int> t;
        if (byEdid.TryGetValue(n, out t)) return t;
        if (n.Length > 1 && (n[0] == 'p' || n[0] == 'P') && byEdid.TryGetValue(n.Substring(1), out t)) return t;
        return null;
    }

    static bool Literal(string a) { int n; float x; return a == "True" || a == "False" || a == "None" || a.StartsWith("\"") || int.TryParse(a, out n) || float.TryParse(a, out x); }

    // The instructions a call can run, in order: with the literal arguments it is called with (null: not known), a branch
    // on a parameter compared to a constant goes one way (MQ302Script.ControlToggle(1) gives the controls back; its 0
    // takes them).
    static List<GearScan.Instr> Reachable(GearScan.Func f, string[] a_args)
    {
        var known = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (int k = 0; a_args != null && k < a_args.Length && k < f.Params.Count; k++) if (a_args[k] != null) known[f.Params[k]] = a_args[k];
        Func<string, string> value = v => Literal(v) ? v : known.ContainsKey(v) ? known[v] : null;
        // a variable written twice isn't known; a compare of two known values is
        var writes = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        foreach (var i in f.Code) if (i.Op != 20 && i.Op != 21 && i.Op != 22 && i.Op != 26 && i.Op != 23 && i.Op != 24 && i.Op != 25 && i.Args.Count > 0) writes[i.Args[0]] = writes.ContainsKey(i.Args[0]) ? writes[i.Args[0]] + 1 : 1;
        foreach (var w in writes) if (w.Value > 0) known.Remove(w.Key);
        foreach (var i in f.Code)
        {
            if (i.Args.Count == 0 || writes.ContainsKey(i.Args[0]) && writes[i.Args[0]] > 1) continue;
            if (i.Op == 15) { var a = value(i.Args[1]); var b = value(i.Args[2]); if (a != null && b != null) known[i.Args[0]] = a.Equals(b, StringComparison.OrdinalIgnoreCase) ? "True" : "False"; }
            if (i.Op == 10) { var a = value(i.Args[1]); if (a == "True" || a == "False") known[i.Args[0]] = a == "True" ? "False" : "True"; }
            if (i.Op == 13) { var a = value(i.Args[1]); if (a != null) known[i.Args[0]] = a; }
        }
        var seen = new bool[f.Code.Count + 1];
        var todo = new Stack<int>(); todo.Push(0);
        while (todo.Count > 0)
        {
            int pc = todo.Pop();
            if (pc < 0 || pc >= f.Code.Count || seen[pc]) continue;
            seen[pc] = true;
            var i = f.Code[pc];
            int off;
            if (i.Op == 26) continue;
            if (i.Op == 20 && int.TryParse(i.Args[0], out off)) { todo.Push(pc + off); continue; }
            if ((i.Op == 21 || i.Op == 22) && int.TryParse(i.Args[1], out off))
            {
                var c = value(i.Args[0]);
                bool jumps = i.Op == 21 ? c == "True" : c == "False", falls = i.Op == 21 ? c == "False" : c == "True";
                if (c == null || (c != "True" && c != "False")) { jumps = true; falls = true; }
                if (jumps) todo.Push(pc + off);
                if (falls) todo.Push(pc + 1);
                continue;
            }
            todo.Push(pc + 1);
        }
        var code = new List<GearScan.Instr>();
        for (int k = 0; k < f.Code.Count; k++) if (seen[k]) code.Add(f.Code[k]);
        return code;
    }

    // What a function does to the player itself (not through what it calls), or null.
    static string Direct(List<GearScan.Instr> a_code)
    {
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Func<string, bool> player = v => players.Contains(v) || Bare(v).IndexOf("PlayerRef", StringComparison.OrdinalIgnoreCase) >= 0;
        foreach (var i in a_code)
        {
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(i.Args[1], "GetPlayer")) { players.Add(i.Args[2]); continue; }
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(i.Args[1], "FadeOutGame") && i.Args.Count > 3 && Is(i.Args[3], "True")) return "FadeOutGame";
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(i.Args[1], "DisablePlayerControls") && (i.Args.Count <= 3 || Is(i.Args[3], "True"))) return "DisablePlayerControls";
            if (i.Op == 13 || i.Op == 14) { if (player(i.Args[1])) players.Add(i.Args[0]); else players.Remove(i.Args[0]); continue; }
            if (i.Op != 23) continue;
            var m = i.Args[0]; var self = i.Args[1];
            if ((Is(m, "GetReference") || Is(m, "GetRef") || Is(m, "GetActorRef") || Is(m, "GetActorReference")) && Bare(self).IndexOf("Player", StringComparison.OrdinalIgnoreCase) >= 0) { players.Add(i.Args[2]); continue; }
            if ((Is(m, "MoveTo") || Is(m, "MoveToNode") || Is(m, "MoveToMyEditorLocation")) && player(self)) return "MoveTo " + Bare(i.Args.Count > 3 ? i.Args[3] : "");
            if (Is(m, "DisablePlayerControls") && (i.Args.Count <= 3 || Is(i.Args[3], "True"))) return "DisablePlayerControls";
            if (Is(m, "EnableMovement") && i.Args.Count > 3 && Is(i.Args[3], "False")) return "EnableMovement(false)";
            if (Is(m, "Activate") && i.Args.Count > 3 && player(i.Args[3]) && Bare(self).IndexOf("Door", StringComparison.OrdinalIgnoreCase) >= 0) return "the player through " + Bare(self);
            players.Remove(i.Args[2]);
        }
        return null;
    }

    // How a function moves the player, itself or through the functions it calls, or null. a_quest: the quest whose
    // script or fragment it is (what its SetStage on self sets), when known.
    static string Moves(GearScan.Func f, string[] a_args, Tuple<GearScan.Quest, int> a_quest, HashSet<string> a_seen)
    {
        var key = f.Obj + "." + f.State + "." + f.Name + "(" + (a_args == null ? "" : string.Join(",", a_args.Select(a => a ?? "?").ToArray())) + ")@" + (a_quest == null ? "" : a_quest.Item2 + ":" + a_quest.Item1.Id);
        string cached;
        if (funcMoves.TryGetValue(key, out cached)) return cached;
        if (!a_seen.Add(key)) return null;
        var code = Reachable(f, a_args);
        string why = Direct(code);
        if (why != null) why = f.Obj + "." + f.Name + ": " + why;
        for (int n = 0; why == null && n < code.Count; n++)
        {
            var i = code[n];
            if (i.Op != 23 && i.Op != 24 && i.Op != 25) continue;
            if (i.Op == 23 && (Is(i.Args[0], "SetStage") || Is(i.Args[0], "Start")))
            {
                var own = Own(i.Args[1]);
                var target = own ? a_quest : QuestOf(i.Args[1]);
                if (target == null) continue;
                var stages = new List<int>();
                int s;
                if (Is(i.Args[0], "SetStage") && i.Args.Count > 3 && int.TryParse(i.Args[3], out s)) stages.Add(s);
                if (!own) stages.AddRange(target.Item1.Startup);  // starts it, if it isn't running
                foreach (var st in stages)
                {
                    var w = Stage(target.Item2, target.Item1, st, a_seen);
                    if (w != null) { why = (own ? "" : target.Item1.Edid + " ") + st + " -> " + w; break; }
                }
                continue;
            }
            string type, name;
            if (i.Op == 25) { type = i.Args[0]; name = i.Args[1]; }
            else if (i.Op == 24) { Script sc; type = scripts.TryGetValue(f.Obj, out sc) ? sc.Obj.Parent : null; name = i.Args[0]; }
            else { type = TypeOf(f, i.Args[1]); name = i.Args[0]; }
            if (type == null) continue;
            string owner;
            var g = Find(type, name, out owner);
            if (g == null) continue;
            // a call on the quest's own script keeps the quest (kmyQuest.StartQuest), another script's doesn't
            var q = (i.Op == 23 && Own(i.Args[1])) || i.Op == 24 ? a_quest : null;
            int first = i.Op == 24 ? 2 : 3;
            var args = i.Args.Skip(first).Select(a => Literal(a) ? a : null).ToArray();
            var w2 = Moves(g, args, q, a_seen);
            if (w2 != null) why = owner + "." + name + " -> " + w2;
        }
        a_seen.Remove(key);
        // only moves are kept: a no found while a caller up the chain was still open may have missed a cycle through it
        if (why != null) funcMoves[key] = why;
        return why;
    }

    static string Stage(int a_file, GearScan.Quest a_quest, int a_stage, HashSet<string> a_seen)
    {
        var key = a_file + ":" + a_quest.Id + ":" + a_stage;
        string cached;
        if (stageMoves.TryGetValue(key, out cached)) return cached;
        if (!a_seen.Add(key)) return null;
        Dictionary<int, List<GearScan.Func>> stages;
        List<GearScan.Func> funcs;
        string why = null;
        if (fragments.TryGetValue(Tuple.Create(a_file, a_quest.Id & 0xFFFFFF), out stages) && stages.TryGetValue(a_stage, out funcs))
            foreach (var f in funcs) { why = Moves(f, null, Tuple.Create(a_quest, a_file), a_seen); if (why != null) break; }
        a_seen.Remove(key);
        if (why != null) stageMoves[key] = why;
        return why;
    }

    public static string Run(string data)
    {
        var sb = new StringBuilder();
        var files = new[] { new { Esm = "Fallout4.esm", Ba2 = "Fallout4 - Misc.ba2" }, new { Esm = "DLCCoast.esm", Ba2 = "DLCCoast - Main.ba2" } };
        var perFile = new List<Dictionary<uint, GearScan.Quest>>();
        for (int file = 0; file < files.Length; file++)
        {
            var d = new Dictionary<uint, GearScan.Quest>();
            foreach (var q in GearScan.Quests(Path.Combine(data, files[file].Esm), file)) { d[q.Id & 0xFFFFFF] = q; if (!byEdid.ContainsKey(q.Edid)) byEdid[q.Edid] = Tuple.Create(q, file); }
            perFile.Add(d);
            foreach (var kv in GearScan.Ba2(Path.Combine(data, files[file].Ba2), n => n.EndsWith(".pex", StringComparison.OrdinalIgnoreCase)))
            {
                List<GearScan.Obj> objs; List<GearScan.Func> funcs;
                try { funcs = GearScan.Pex(kv.Value, out objs); } catch { continue; }
                foreach (var o in objs) scripts[o.Name] = new Script { Obj = o };
                foreach (var f in funcs) { Script s; if (scripts.TryGetValue(f.Obj, out s) && !s.Funcs.ContainsKey(f.Name)) s.Funcs[f.Name] = f; }
                if (kv.Key.IndexOf(@"\fragments\quests\", StringComparison.OrdinalIgnoreCase) < 0) continue;
                var name = Path.GetFileNameWithoutExtension(kv.Key);
                uint id;
                if (!uint.TryParse(name.Substring(name.LastIndexOf('_') + 1), System.Globalization.NumberStyles.HexNumber, null, out id) || !d.ContainsKey(id & 0xFFFFFF)) continue;
                var key = Tuple.Create(file, id & 0xFFFFFF);
                if (!fragments.ContainsKey(key)) fragments[key] = new Dictionary<int, List<GearScan.Func>>();
                foreach (var f in funcs)
                {
                    int stage;
                    if (!f.Name.StartsWith("Fragment_Stage_", StringComparison.OrdinalIgnoreCase) || !int.TryParse(f.Name.Substring("Fragment_Stage_".Length, 4), out stage)) continue;
                    if (!fragments[key].ContainsKey(stage)) fragments[key][stage] = new List<GearScan.Func>();
                    fragments[key][stage].Add(f);
                }
            }
        }
        var lines = new SortedSet<string>();
        foreach (var kv in fragments)
        {
            var quest = perFile[kv.Key.Item1][kv.Key.Item2];
            if (QuestScan.Shared(quest, kv.Key.Item1) == "not shared") continue;
            foreach (var st in kv.Value.Keys)
            {
                var why = Stage(kv.Key.Item1, quest, st, new HashSet<string>());
                if (why != null) lines.Add(string.Format("{0} {1:X6} {2,5} {3,-34} {4}", kv.Key.Item1, quest.Id & 0xFFFFFF, st, quest.Edid, why));
            }
        }
        foreach (var l in lines) sb.AppendLine(l);
        return sb.ToString();
    }
}

// The enable parents (a reference some placed reference names in its XESP) that a script of the game's own turns off
// somewhere: what src/game/DisabledParents.h holds (tools/questscan.ps1 -DisabledParents writes it). Read from the
// official plugins found (Fallout4.esm and the add-ons) and their script archives. A script turns a reference off when
// it calls Disable or DisableNoWait on it (TryToDisable or TryToDisableNoWait on an alias forced to it, DisableLinkChain
// on what links to it): through a property that names it, an alias property whose alias is forced to it (or to an alias
// of another quest that is), itself (a script attached to it, to its base or to an alias forced to it), its linked
// reference (by the keyword it is called with, where that is known), or by passing one of those to a function, a few
// calls deep, that does. A property is read from every script attached to a record (VMAD): a quest's own, its stage
// fragments' and its aliases', a reference's (with its base's), a base object's, a topic's, a scene's, a package's, a
// perk's and a terminal's. Each attachment is judged with the values its properties have there: a branch on a bool,
// number or string property that no script ever sets goes the one way those values send it (DefaultEnableDisableTrigScript
// with bEnableLinkedRef only enables), any other branch both ways. Over-counted on purpose: any function of a script,
// whether a game ever calls it or not. What it does not see: a reference reached through a variable set at run time, a
// struct's member, a collection alias, an alias filled by conditions rather than forced, a console command.
public static class ParentScan
{
    // the game's own plugins in load order, as Official.h names them, and the archive that holds each one's scripts
    public static readonly string[] FILES = { "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm", "DLCNukaWorld.esm" };
    static string ScriptArchive(string a_esm) { return a_esm == "Fallout4.esm" ? "Fallout4 - Misc.ba2" : Path.GetFileNameWithoutExtension(a_esm) + " - Main.ba2"; }

    static readonly HashSet<string> PLACED = new HashSet<string> { "REFR", "ACHR", "PGRE", "PMIS", "PARW", "PBAR", "PBEA", "PCON", "PFLA", "PHZD" };
    static readonly HashSet<string> SKIP = new HashSet<string> { "LAND", "NAVM", "NAVI", "WRLD", "CELL", "LCTN", "IMAD", "LSCR" };
    static readonly HashSet<string> FRAGMENTS = new HashSet<string> { "QUST", "INFO", "PACK", "SCEN", "PERK", "TERM" };

    class Ref { public string Edid = ""; public uint Base; public uint Parent; public uint Flags; public bool Actor; public List<Tuple<uint, uint>> Links = new List<Tuple<uint, uint>>(); }  // links: keyword, reference
    class Prop { public string Name; public string Literal; public List<uint> Forms = new List<uint>(); public List<int> Aliases = new List<int>(); }
    class Attached { public string Script; public List<Prop> Props = new List<Prop>(); }
    // a script attached somewhere: what self is there, and where, for the table's comments
    class Attachment { public Attached Entry; public string Where; public uint Self; public uint SelfQuest; public int SelfAlias = -1; }

    static readonly Dictionary<uint, Ref> refs = new Dictionary<uint, Ref>();
    static readonly Dictionary<uint, Dictionary<int, uint>> forced = new Dictionary<uint, Dictionary<int, uint>>();      // quest -> alias -> forced reference
    static readonly Dictionary<uint, Dictionary<int, Tuple<uint, int>>> external = new Dictionary<uint, Dictionary<int, Tuple<uint, int>>>();  // quest -> alias -> another quest's alias
    static readonly List<Attachment> attachments = new List<Attachment>();
    static readonly Dictionary<uint, Tuple<string, List<Attached>>> baseScripts = new Dictionary<uint, Tuple<string, List<Attached>>>();
    static readonly Dictionary<uint, Tuple<string, List<Attached>>> refScripts = new Dictionary<uint, Tuple<string, List<Attached>>>();
    static int vmadRead, vmadErrors;
    static readonly List<string> vmadErrorWhere = new List<string>();

    static uint Key(uint a_id, List<string> a_masters, string a_self)
    {
        int high = (int)(a_id >> 24);
        string file = high < a_masters.Count ? a_masters[high] : a_self;
        int index = Array.FindIndex(FILES, f => f.Equals(file, StringComparison.OrdinalIgnoreCase));
        if (index < 0) return 0xFFFFFFFF;
        return (uint)index << 24 | (a_id & 0xFFFFFF);
    }

    // ---------------- VMAD ----------------
    class V
    {
        public byte[] B; public int P; public int End; public int Format = 2;
        public byte U8() { return B[P++]; }
        public short S16() { var v = BitConverter.ToInt16(B, P); P += 2; return v; }
        public ushort U16() { var v = BitConverter.ToUInt16(B, P); P += 2; return v; }
        public uint U32() { var v = BitConverter.ToUInt32(B, P); P += 4; return v; }
        public string W() { int n = U16(); var s = Encoding.ASCII.GetString(B, P, n); P += n; return s; }
        public void Obj(out uint a_form, out int a_alias)
        {
            if (Format == 1) { a_form = U32(); a_alias = S16(); U16(); }
            else { U16(); a_alias = S16(); a_form = U32(); }
        }
    }

    // A property's value; a single bool, number or string also as the script reader writes a literal (True, 3, "text").
    static void Value(V v, int a_type, Prop a_prop, bool a_single)
    {
        uint form; int alias;
        switch (a_type)
        {
            case 0: break;
            case 1: v.Obj(out form, out alias); a_prop.Forms.Add(form); a_prop.Aliases.Add(alias); break;
            case 2: { var s = v.W(); if (a_single) a_prop.Literal = "\"" + s + "\""; break; }
            case 3: { var n = (int)v.U32(); if (a_single) a_prop.Literal = n.ToString(); break; }
            case 4: { var x = BitConverter.ToSingle(v.B, v.P); v.P += 4; if (a_single) a_prop.Literal = x.ToString(System.Globalization.CultureInfo.InvariantCulture); break; }
            case 5: { var b = v.U8(); if (a_single) a_prop.Literal = b != 0 ? "True" : "False"; break; }
            case 6: { int t = v.U8(); Value(v, t, a_prop, false); break; }  // a Var: its own type, then its value
            case 7: { uint n = v.U32(); for (uint k = 0; k < n; k++) Property(v); break; }  // a struct's members: not followed
            case 11: case 12: case 13: case 14: case 15: case 16:
                { uint n = v.U32(); for (uint k = 0; k < n; k++) Value(v, a_type - 10, a_prop, false); break; }
            case 17: { uint n = v.U32(); for (uint k = 0; k < n; k++) { uint m = v.U32(); for (uint j = 0; j < m; j++) Property(v); } break; }
            default: throw new Exception("property type " + a_type);
        }
    }

    static Prop Property(V v)
    {
        var p = new Prop { Name = v.W() };
        int type = v.U8(); v.U8();
        Value(v, type, p, true);
        return p;
    }

    // a script and its properties; null for a fragment script entry without a script (its empty name only)
    static Attached ReadScript(V v)
    {
        var s = new Attached { Script = v.W() };
        if (s.Script.Length == 0) return null;
        v.U8();
        int n = v.U16();
        for (int k = 0; k < n; k++) s.Props.Add(Property(v));
        return s;
    }

    static List<Attached> Scripts(V v)
    {
        var list = new List<Attached>();
        int n = v.U16();
        for (int k = 0; k < n; k++) list.Add(ReadScript(v));
        return list;
    }

    // A record's VMAD: its scripts, and a quest's, a topic's, a scene's, a package's, a perk's or a terminal's fragment
    // script, and a quest's aliases' scripts. Every one is read to its end, but for the fragments' names.
    static void Vmad(string a_type, uint a_key, byte[] a_body, int a_at, int a_size, List<string> a_masters, string a_file, string a_where)
    {
        var v = new V { B = a_body, P = a_at, End = a_at + a_size };
        var own = new List<Attached>();
        var found = new List<Attachment>();
        try
        {
            v.S16(); v.Format = v.S16();
            own = Scripts(v);
            if (v.P < v.End && FRAGMENTS.Contains(a_type))
            {
                v.U8();
                if (a_type == "QUST")
                {
                    int frags = v.U16();
                    var e = ReadScript(v);
                    if (e != null) found.Add(new Attachment { Entry = e, Where = a_where + " (stage fragments)" });
                    for (int k = 0; k < frags; k++) { v.U16(); v.S16(); v.U32(); v.U8(); v.W(); v.W(); }
                    int aliases = v.U16();
                    for (int k = 0; k < aliases; k++)
                    {
                        uint form; int alias; v.Obj(out form, out alias);
                        v.S16(); v.Format = v.S16();
                        foreach (var s in Scripts(v)) found.Add(new Attachment { Entry = s, Where = a_where + " alias " + alias, SelfQuest = a_key, SelfAlias = alias });
                    }
                }
                else
                {
                    if (a_type == "INFO" || a_type == "PACK" || a_type == "SCEN") v.U8();  // which fragments there are
                    var e = ReadScript(v);
                    if (e != null) found.Add(new Attachment { Entry = e, Where = a_where + " (fragments)" });
                    v.P = v.End;  // the fragments' names follow: nothing more is attached
                }
            }
            if (v.P != v.End) throw new Exception("ended at " + (v.P - a_at) + " of " + a_size);
            vmadRead++;
        }
        catch (Exception ex)
        {
            vmadErrors++;
            if (vmadErrorWhere.Count < 20) vmadErrorWhere.Add(a_where + ": " + ex.Message);
        }
        Action<Attached> keys = s => { foreach (var p in s.Props) for (int k = 0; k < p.Forms.Count; k++) p.Forms[k] = Key(p.Forms[k], a_masters, a_file); };
        foreach (var s in own) keys(s);
        foreach (var a in found) { keys(a.Entry); attachments.Add(a); }
        if (PLACED.Contains(a_type)) refScripts[a_key] = Tuple.Create(a_where, own);  // judged with its base's, below
        else if (FRAGMENTS.Contains(a_type)) foreach (var s in own) attachments.Add(new Attachment { Entry = s, Where = a_where });
        else baseScripts[a_key] = Tuple.Create(a_where, own);
    }

    static void Esm(string a_path, string a_file)
    {
        var all = File.ReadAllBytes(a_path);
        var masters = new List<string>();
        uint hsize = BitConverter.ToUInt32(all, 4);
        for (int s = 24; s < 24 + hsize;)
        {
            string st = Encoding.ASCII.GetString(all, s, 4); int ss = BitConverter.ToUInt16(all, s + 4);
            if (st == "MAST") masters.Add(Encoding.ASCII.GetString(all, s + 6, ss - 1));
            s += 6 + ss;
        }
        int pos = 24 + (int)hsize;
        while (pos < all.Length)
        {
            string type = Encoding.ASCII.GetString(all, pos, 4);
            uint size = BitConverter.ToUInt32(all, pos + 4);
            if (type == "GRUP") { pos += 24; continue; }  // its records follow
            uint flags = BitConverter.ToUInt32(all, pos + 8);
            uint id = BitConverter.ToUInt32(all, pos + 12);
            int next = pos + 24 + (int)size;
            uint key = Key(id, masters, a_file);
            bool placed = PLACED.Contains(type);
            if ((flags & 0x20) != 0) { if (placed) { refs.Remove(key); refScripts.Remove(key); } pos = next; continue; }  // 0x20: deleted
            if (SKIP.Contains(type)) { pos = next; continue; }
            byte[] body; int bo; int bl;
            if ((flags & 0x40000) != 0)
            {
                int dsize = (int)BitConverter.ToUInt32(all, pos + 24);
                body = GearScan.Inflate(all, pos + 28, (int)size - 4, dsize); bo = 0; bl = dsize;
            }
            else { body = all; bo = pos + 24; bl = (int)size; }
            Ref r = null;
            if (placed) { r = new Ref { Flags = flags, Actor = type == "ACHR" }; refs[key] = r; refScripts.Remove(key); }  // a later file's override replaces it
            int questAlias = -1;
            string edid = "";
            int vmadAt = -1, vmadSize = 0;
            int big = -1;
            for (int s = bo; s < bo + bl;)
            {
                string st = Encoding.ASCII.GetString(body, s, 4);
                int ss = BitConverter.ToUInt16(body, s + 4);
                if (big >= 0) { ss = big; big = -1; }
                if (st == "XXXX") { big = (int)BitConverter.ToUInt32(body, s + 6); s += 10; continue; }
                int d = s + 6;
                if (st == "EDID") edid = Encoding.ASCII.GetString(body, d, Math.Max(0, ss - 1));
                else if (st == "VMAD") { vmadAt = d; vmadSize = ss; }
                else if (placed && st == "NAME") r.Base = Key(BitConverter.ToUInt32(body, d), masters, a_file);
                else if (placed && st == "XESP") r.Parent = Key(BitConverter.ToUInt32(body, d), masters, a_file);
                else if (placed && st == "XLKR" && ss >= 8)
                {
                    uint keyword = BitConverter.ToUInt32(body, d);
                    r.Links.Add(Tuple.Create(keyword == 0 ? 0u : Key(keyword, masters, a_file), Key(BitConverter.ToUInt32(body, d + 4), masters, a_file)));
                }
                else if (type == "QUST" && st == "ALST") questAlias = (int)BitConverter.ToUInt32(body, d);
                else if (type == "QUST" && (st == "ALLS" || st == "ALED")) questAlias = -1;
                else if (type == "QUST" && st == "ALFR" && questAlias >= 0)
                {
                    if (!forced.ContainsKey(key)) forced[key] = new Dictionary<int, uint>();
                    forced[key][questAlias] = Key(BitConverter.ToUInt32(body, d), masters, a_file);
                }
                else if (type == "QUST" && st == "ALEQ" && questAlias >= 0)
                {
                    if (!external.ContainsKey(key)) external[key] = new Dictionary<int, Tuple<uint, int>>();
                    external[key][questAlias] = Tuple.Create(Key(BitConverter.ToUInt32(body, d), masters, a_file), -1);
                }
                else if (type == "QUST" && st == "ALEA" && questAlias >= 0 && external.ContainsKey(key) && external[key].ContainsKey(questAlias))
                    external[key][questAlias] = Tuple.Create(external[key][questAlias].Item1, (int)BitConverter.ToUInt32(body, d));
                s = d + ss;
            }
            if (placed) r.Edid = edid;
            if (vmadAt >= 0) Vmad(type, key, body, vmadAt, vmadSize, masters, a_file, type + " " + (edid.Length > 0 ? edid : key.ToString("X8")));
            pos = next;
        }
    }

    // ---------------- scripts ----------------
    class Script { public GearScan.Obj Obj; public List<GearScan.Func> Funcs = new List<GearScan.Func>(); public Dictionary<string, GearScan.Func> ByName = new Dictionary<string, GearScan.Func>(StringComparer.OrdinalIgnoreCase); }
    static readonly Dictionary<string, Script> scripts = new Dictionary<string, Script>(StringComparer.OrdinalIgnoreCase);
    // what each function turns off, by where it came from: -1 itself (self), k its k-th parameter
    static readonly Dictionary<GearScan.Func, HashSet<int>> summaries = new Dictionary<GearScan.Func, HashSet<int>>();
    // another script's properties a function turns off (kmyQuest.Marker.Disable()), by that script
    static readonly Dictionary<string, HashSet<string>> foreign = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
    // the properties some script sets (propset, by name, on any script): never read as the constants they start as
    static readonly HashSet<string> setByAnyone = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

    static readonly HashSet<string> OFF = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "Disable", "DisableNoWait", "TryToDisable", "TryToDisableNoWait" };
    static readonly HashSet<string> GETREF = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "GetReference", "GetRef", "GetActorRef", "GetActorReference" };

    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }
    static bool Number(string a, out double a_value) { return double.TryParse(a, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out a_value); }
    static bool Literal(string a) { double n; return a == "True" || a == "False" || a == "None" || a.StartsWith("\"") || Number(a, out n); }

    static GearScan.Func Find(string a_type, string a_func)
    {
        foreach (var s in Chain(a_type)) { GearScan.Func f; if (s.ByName.TryGetValue(a_func, out f)) return f; }
        return null;
    }

    static List<Script> Chain(string a_type)
    {
        var list = new List<Script>();
        Script s;
        for (int depth = 0; !string.IsNullOrEmpty(a_type) && depth < 16 && scripts.TryGetValue(a_type, out s); depth++) { list.Add(s); a_type = s.Obj.Parent; }
        return list;
    }

    static string TypeOf(GearScan.Func f, string a_var)
    {
        if (Is(a_var, "self")) return f.Obj;
        string t;
        if (f.Types.TryGetValue(a_var, out t)) return t;
        foreach (var s in Chain(f.Obj)) if (s.Obj.Types.TryGetValue(a_var, out t) || s.Obj.Types.TryGetValue(Bare(a_var), out t)) return t;
        return null;
    }

    // Where a variable's value can come from, in one function (all its writes, wherever they are): "S" self, "A<k>" its
    // k-th parameter, "P<name>" a property or variable of its script, "F<script>.<name>" another script's property,
    // "L<keyword>:" + one of those, what links from it by that keyword ("0" none, "P<name>" a property's, "*" any).
    static Dictionary<string, HashSet<string>> Sources(GearScan.Func f)
    {
        var src = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
        for (int pass = 0; pass < 3; pass++)
            foreach (var i in f.Code)
            {
                if ((i.Op == 13 || i.Op == 14 || i.Op == 32) && i.Args.Count >= 2) Into(src, i.Args[0], Of(f, src, i.Args[1]));  // 32: an array's element
                else if (i.Op == 28 && i.Args.Count >= 3)  // propget name, object, dest
                {
                    if (Is(i.Args[1], "self")) Into(src, i.Args[2], new[] { "P" + i.Args[0] });
                    else { var t = TypeOf(f, i.Args[1]); if (t != null) Into(src, i.Args[2], new[] { "F" + t + "." + i.Args[0] }); }
                }
                else if (i.Op == 23 && i.Args.Count >= 3)
                {
                    if (GETREF.Contains(i.Args[0])) Into(src, i.Args[2], Of(f, src, i.Args[1]));
                    else if (Is(i.Args[0], "GetLinkedRef")) Into(src, i.Args[2], Linked(f, src, i));
                }
            }
        return src;
    }

    static void Into(Dictionary<string, HashSet<string>> a_src, string a_dst, IEnumerable<string> a_from)
    {
        HashSet<string> set;
        if (!a_src.TryGetValue(a_dst, out set)) a_src[a_dst] = set = new HashSet<string>();
        set.UnionWith(a_from);
    }

    static HashSet<string> Of(GearScan.Func f, Dictionary<string, HashSet<string>> a_src, string a)
    {
        var set = new HashSet<string>();
        if (Is(a, "self")) { set.Add("S"); return set; }
        int k = f.Params.FindIndex(p => Is(p, a));
        if (k >= 0) set.Add("A" + k);
        if (a.StartsWith("::")) set.Add("P" + Bare(a));
        HashSet<string> known;
        if (a_src.TryGetValue(a, out known)) set.UnionWith(known);
        return set;
    }

    // what a GetLinkedRef or DisableLinkChain call's object links to, by the keyword it is called with
    static List<string> Linked(GearScan.Func f, Dictionary<string, HashSet<string>> a_src, GearScan.Instr i)
    {
        var kw = i.Args.Count <= 3 || Is(i.Args[3], "None") ? "0" : i.Args[3].StartsWith("::") ? "P" + Bare(i.Args[3]) : "*";
        return Of(f, a_src, i.Args[1]).Where(s => !s.StartsWith("L")).Select(s => "L" + kw + ":" + s).ToList();
    }

    // What a script's functions turn off, by where it came from (Sources' names; "C" + a link: the chain of links from it).
    class Sink { public HashSet<string> Off = new HashSet<string>(); }

    // The instructions a function can run, with the values a_known gives some of its variables (the properties no
    // script sets, as the attachment has them): a branch on a known value goes one way, any other both. Path by path
    // (a value set on one way is known on it); null when there are too many paths: all of them can run.
    static HashSet<int> Reach(GearScan.Func f, Dictionary<string, string> a_known)
    {
        if (a_known.Count == 0) return null;
        var reached = new HashSet<int>();
        var seen = new HashSet<string>();
        var todo = new Stack<Tuple<int, Dictionary<string, string>>>();
        todo.Push(Tuple.Create(0, new Dictionary<string, string>(a_known, StringComparer.OrdinalIgnoreCase)));
        int steps = 0;
        while (todo.Count > 0)
        {
            var t = todo.Pop(); int pc = t.Item1; var st = t.Item2;
            while (pc >= 0 && pc < f.Code.Count)
            {
                if (++steps > 200000) return null;
                if (!seen.Add(pc + "|" + string.Join(",", st.Where(kv => !a_known.ContainsKey(kv.Key)).OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value).ToArray()))) break;
                reached.Add(pc);
                var i = f.Code[pc];
                Func<string, string> val = a => Literal(a) ? a : st.ContainsKey(a) ? st[a] : null;
                Action<string, string> set = (dst, v) => { if (a_known.ContainsKey(dst)) return; if (v == null) st.Remove(dst); else st[dst] = v; };
                int off;
                if (i.Op == 26) break;
                if (i.Op == 20 && int.TryParse(i.Args[0], out off)) { pc += off; continue; }
                if ((i.Op == 21 || i.Op == 22) && int.TryParse(i.Args[1], out off))
                {
                    var c = val(i.Args[0]);
                    if (c == "True" || c == "False") { pc += (c == "True") == (i.Op == 21) ? off : 1; continue; }
                    // on each way the condition is known (a && b's false: the jump past b, with the condition false)
                    var jumped = new Dictionary<string, string>(st, StringComparer.OrdinalIgnoreCase);
                    if (!Literal(i.Args[0]) && !a_known.ContainsKey(i.Args[0])) { jumped[i.Args[0]] = i.Op == 21 ? "True" : "False"; st[i.Args[0]] = i.Op == 21 ? "False" : "True"; }
                    todo.Push(Tuple.Create(pc + off, jumped));
                    pc++;
                    continue;
                }
                if (i.Op == 13 || i.Op == 14)
                {
                    var v = val(i.Args[1]);
                    if (i.Op == 14 && v != null && v != "True" && v != "False")
                    {
                        string type; double n;
                        v = f.Types.TryGetValue(i.Args[0], out type) && Is(type, "Bool") && Number(v, out n) ? (n != 0 ? "True" : "False") : null;
                    }
                    set(i.Args[0], v);
                }
                else if (i.Op == 10) { var v = val(i.Args[1]); set(i.Args[0], v == "True" ? "False" : v == "False" ? "True" : null); }
                else if (i.Op >= 15 && i.Op <= 19)
                {
                    var a = val(i.Args[1]); var b = val(i.Args[2]); string r = null; double x, y;
                    if (a != null && b != null)
                    {
                        if (i.Op == 15) r = (Number(a, out x) && Number(b, out y) ? x == y : a.Equals(b, StringComparison.OrdinalIgnoreCase)) ? "True" : "False";
                        else if (Number(a, out x) && Number(b, out y)) r = (i.Op == 16 ? x < y : i.Op == 17 ? x <= y : i.Op == 18 ? x > y : x >= y) ? "True" : "False";
                    }
                    set(i.Args[0], r);
                }
                else foreach (var a in i.Args) if (!Literal(a)) set(a, null);  // what else it writes isn't known
                pc++;
            }
        }
        return reached;
    }

    // One function, over the instructions a_reach says can run (null: all): what it turns off into a_sink, and, with
    // a_summary, what of its own (self, a parameter) it turns off. True if a_summary grew.
    static bool Analyze(GearScan.Func f, HashSet<int> a_reach, Sink a_sink, HashSet<int> a_summary)
    {
        var src = Sources(f);
        bool changed = false;
        Action<IEnumerable<string>> mark = from =>
        {
            foreach (var s in from)
            {
                if (s == "S" && a_summary != null) changed |= a_summary.Add(-1);
                else if (s.StartsWith("A")) { if (a_summary != null) changed |= a_summary.Add(int.Parse(s.Substring(1))); continue; }
                else if (s.StartsWith("F"))
                {
                    int dot = s.IndexOf('.');
                    HashSet<string> h;
                    if (!foreign.TryGetValue(s.Substring(1, dot - 1), out h)) foreign[s.Substring(1, dot - 1)] = h = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    h.Add(s.Substring(dot + 1));
                    continue;
                }
                a_sink.Off.Add(s);
            }
        };
        for (int n = 0; n < f.Code.Count; n++)
        {
            if (a_reach != null && !a_reach.Contains(n)) continue;
            var i = f.Code[n];
            if (i.Op == 23 && i.Args.Count >= 3 && OFF.Contains(i.Args[0])) { mark(Of(f, src, i.Args[1])); continue; }
            if (i.Op == 23 && i.Args.Count >= 3 && Is(i.Args[0], "DisableLinkChain")) { mark(Linked(f, src, i).Select(s => "C" + s)); continue; }
            if (i.Op != 23 && i.Op != 24 && i.Op != 25) continue;
            string type, name, self = null;
            int first;
            if (i.Op == 25) { type = i.Args[0]; name = i.Args[1]; first = 3; }
            else if (i.Op == 24) { Script sc; type = scripts.TryGetValue(f.Obj, out sc) ? sc.Obj.Parent : null; name = i.Args[0]; first = 2; self = "self"; }
            else { type = TypeOf(f, i.Args[1]); name = i.Args[0]; first = 3; self = i.Args[1]; }
            if (type == null) continue;
            var g = Find(type, name);
            HashSet<int> callee;
            if (g == null || !summaries.TryGetValue(g, out callee)) continue;
            foreach (var k in callee.ToList())
            {
                if (k == -1) { if (self != null) mark(Of(f, src, self)); }
                else if (first + k < i.Args.Count) mark(Of(f, src, i.Args[first + k]));
            }
        }
        return changed;
    }

    static uint Forced(uint a_quest, int a_alias, int a_depth)
    {
        Dictionary<int, uint> f; uint r;
        if (forced.TryGetValue(a_quest, out f) && f.TryGetValue(a_alias, out r)) return r;
        Dictionary<int, Tuple<uint, int>> e; Tuple<uint, int> t;
        if (a_depth < 8 && external.TryGetValue(a_quest, out e) && e.TryGetValue(a_alias, out t) && t.Item2 >= 0) return Forced(t.Item1, t.Item2, a_depth + 1);
        return 0;
    }

    // What a script turns off with the property values it has (the bool, number and string ones no script sets), cached.
    static readonly Dictionary<string, Sink> judged = new Dictionary<string, Sink>(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, HashSet<string>> writtenIn = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
    static Sink Judge(string a_script, List<Prop> a_props)
    {
        var chain = Chain(a_script);
        var known = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var s in chain)
        {
            HashSet<string> written;
            if (!writtenIn.TryGetValue(s.Obj.Name, out written))
            {
                // every variable an instruction of the script writes (or may: a call's arguments are passed by value)
                written = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var f in s.Funcs)
                    foreach (var i in f.Code)
                        if (i.Op == 20 || i.Op == 21 || i.Op == 22 || i.Op == 26 || i.Op == 29 || i.Op == 33 || i.Op == 39) continue;
                        else if (i.Op == 23 || i.Op == 25) { if (i.Args.Count > 2) written.Add(i.Args[2]); }
                        else if (i.Op == 24 || i.Op == 28) { if (i.Args.Count > 2) written.Add(i.Args[i.Op == 24 ? 1 : 2]); }
                        else if (i.Op == 34 || i.Op == 35 || i.Op == 40 || i.Op == 41) { if (i.Args.Count > 1) written.Add(i.Args[1]); }
                        else if (i.Args.Count > 0) written.Add(i.Args[0]);
                writtenIn[s.Obj.Name] = written;
            }
            foreach (var d in s.Obj.Defaults)
            {
                if (!d.Key.StartsWith("::") || written.Contains(d.Key) || setByAnyone.Contains(Bare(d.Key))) continue;
                string type;
                if (!s.Obj.Types.TryGetValue(d.Key, out type) || !(Is(type, "Bool") || Is(type, "Int") || Is(type, "Float") || Is(type, "String"))) continue;
                var p = a_props.FirstOrDefault(x => Is(x.Name, Bare(d.Key)));
                var v = p != null ? p.Literal : d.Value;
                if (v != null && v != "None") known[d.Key] = v;
            }
        }
        var cacheKey = a_script + "|" + string.Join(",", known.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value).ToArray());
        Sink sink;
        if (judged.TryGetValue(cacheKey, out sink)) return sink;
        sink = new Sink();
        foreach (var s in chain)
        {
            foreach (var f in s.Funcs) Analyze(f, Reach(f, known), sink, null);
            HashSet<string> h;
            if (foreign.TryGetValue(s.Obj.Name, out h)) foreach (var n in h) sink.Off.Add("P" + n);
        }
        judged[cacheKey] = sink;
        return sink;
    }

    public class Result { public string Header; public string Report; public int Count; }

    public static Result Run(string a_data)
    {
        var report = new StringBuilder();
        var scanned = new List<string>();
        foreach (var file in FILES)
        {
            var esm = Path.Combine(a_data, file);
            if (!File.Exists(esm)) { report.AppendLine("# not found, not scanned: " + file); continue; }
            Esm(esm, file);
            scanned.Add(file);
            var ba2 = Path.Combine(a_data, ScriptArchive(file));
            if (!File.Exists(ba2)) { report.AppendLine("# no script archive for " + file); continue; }
            int n = 0;
            foreach (var kv in GearScan.Ba2(ba2, x => x.EndsWith(".pex", StringComparison.OrdinalIgnoreCase)))
            {
                List<GearScan.Obj> objs; List<GearScan.Func> funcs;
                try { funcs = GearScan.Pex(kv.Value, out objs); } catch { continue; }
                foreach (var o in objs) scripts[o.Name] = new Script { Obj = o };  // a later archive's replaces it
                foreach (var f in funcs)
                {
                    Script s;
                    if (!scripts.TryGetValue(f.Obj, out s)) continue;
                    s.Funcs.Add(f);
                    if (!s.ByName.ContainsKey(f.Name)) s.ByName[f.Name] = f;
                }
                n++;
            }
            report.AppendLine(string.Format("# {0}: {1} scripts", file, n));
        }
        foreach (var s in scripts.Values) foreach (var f in s.Funcs) { summaries[f] = new HashSet<int>(); foreach (var i in f.Code) if (i.Op == 29 && i.Args.Count > 0) setByAnyone.Add(i.Args[0]); }
        for (int pass = 0; pass < 12; pass++)
        {
            bool changed = false;
            var scratch = new Sink();
            foreach (var s in scripts.Values) foreach (var f in s.Funcs) changed |= Analyze(f, null, scratch, summaries[f]);
            if (!changed) { report.AppendLine("# calls followed in " + (pass + 1) + " passes"); break; }
        }

        // a reference's scripts: its base's, with the properties the reference sets over them, and its own
        foreach (var kv in refs)
        {
            Tuple<string, List<Attached>> b, own;
            baseScripts.TryGetValue(kv.Value.Base, out b);
            refScripts.TryGetValue(kv.Key, out own);
            if (b == null && own == null) continue;
            var byName = new Dictionary<string, Attached>(StringComparer.OrdinalIgnoreCase);
            var where = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (b != null) foreach (var s in b.Item2) { byName[s.Script] = new Attached { Script = s.Script, Props = new List<Prop>(s.Props) }; where[s.Script] = b.Item1 + " (its base)"; }
            if (own != null)
                foreach (var s in own.Item2)
                {
                    Attached merged;
                    if (!byName.TryGetValue(s.Script, out merged)) { byName[s.Script] = s; where[s.Script] = own.Item1; continue; }
                    foreach (var p in s.Props) { merged.Props.RemoveAll(x => Is(x.Name, p.Name)); merged.Props.Add(p); }
                    where[s.Script] = own.Item1;
                }
            foreach (var s in byName.Values) attachments.Add(new Attachment { Entry = s, Where = where[s.Script], Self = kv.Key });
        }

        var children = new Dictionary<uint, int>();
        var actors = new Dictionary<uint, int>();
        foreach (var r in refs.Values) if (r.Parent != 0 && r.Parent != 0xFFFFFFFF) { children[r.Parent] = (children.ContainsKey(r.Parent) ? children[r.Parent] : 0) + 1; if (r.Actor) actors[r.Parent] = (actors.ContainsKey(r.Parent) ? actors[r.Parent] : 0) + 1; }
        var why = new Dictionary<uint, List<string>>();
        Action<uint, string> off = (id, w) => { if (id == 0 || id == 0xFFFFFFFF) return; List<string> l; if (!why.TryGetValue(id, out l)) why[id] = l = new List<string>(); if (!l.Contains(w)) l.Add(w); };
        int judgedCount = 0;
        foreach (var a in attachments)
        {
            if (!scripts.ContainsKey(a.Entry.Script)) continue;
            uint self = a.Self != 0 ? a.Self : a.SelfAlias >= 0 ? Forced(a.SelfQuest, a.SelfAlias, 0) : 0;
            var sink = Judge(a.Entry.Script, a.Entry.Props);
            judgedCount++;
            if (sink.Off.Count == 0) continue;
            var entry = a.Entry;
            // what an origin ("S", "P<name>") names here
            Func<string, List<uint>> named = o =>
            {
                if (o == "S") return self != 0 ? new List<uint> { self } : new List<uint>();
                var p = o.StartsWith("P") ? entry.Props.FirstOrDefault(x => Is(x.Name, o.Substring(1))) : null;
                if (p == null) return new List<uint>();
                return p.Forms.Select((form, k) => p.Aliases[k] >= 0 ? Forced(form, p.Aliases[k], 0) : form).ToList();
            };
            // what links from a reference by a keyword ("0" none, "P<name>" a keyword property's, "*" any)
            Func<uint, string, List<uint>> links = (id, kw) =>
            {
                Ref r;
                if (!refs.TryGetValue(id, out r)) return new List<uint>();
                if (kw == "*") return r.Links.Select(l => l.Item2).ToList();
                uint keyword = 0;
                if (kw.StartsWith("P"))
                {
                    var p = entry.Props.FirstOrDefault(x => Is(x.Name, kw.Substring(1)));
                    if (p != null && p.Forms.Count > 0) keyword = p.Forms[0];
                }
                return r.Links.Where(l => l.Item1 == keyword).Select(l => l.Item2).ToList();
            };
            foreach (var o in sink.Off)
            {
                var w = a.Where + ": " + entry.Script;
                if (o == "S" || o.StartsWith("P")) { foreach (var id in named(o)) off(id, w + (o == "S" ? " on itself" : "." + o.Substring(1))); continue; }
                if (!o.StartsWith("L") && !o.StartsWith("C")) continue;
                bool chainOf = o.StartsWith("C");
                var rest = chainOf ? o.Substring(1) : o;  // L<keyword>:<origin>
                int colon = rest.IndexOf(':');
                var kw = rest.Substring(1, colon - 1);
                var origin = rest.Substring(colon + 1);
                var from = named(origin);
                for (int depth = 0; depth < (chainOf ? 32 : 1) && from.Count > 0; depth++)
                {
                    var next = new List<uint>();
                    foreach (var id in from)
                        foreach (var l in links(id, kw)) { off(l, w + (origin == "S" ? " on its linked ref" : " on " + origin.Substring(1) + "'s linked ref") + (chainOf ? " chain" : "")); next.Add(l); }
                    from = next;
                }
            }
        }

        var table = why.Keys.Where(id => children.ContainsKey(id)).OrderBy(id => id).ToList();
        int offInPlugin = table.Count(id => refs.ContainsKey(id) && (refs[id].Flags & 0x800) != 0);
        report.AppendLine(string.Format("# {0} references; {1} enable parents; {2} turned off by a script, {3} of them enable parents ({4} off in their plugin)", refs.Count, children.Count, why.Count, table.Count, offInPlugin));
        report.AppendLine(string.Format("# {0} script attachments judged ({1} sets of property values); {2} VMADs read, {3} not read to their end", judgedCount, judged.Count, vmadRead, vmadErrors));
        foreach (var e in vmadErrorWhere) report.AppendLine("#   " + e);

        var h = new StringBuilder();
        h.AppendLine("#pragma once");
        h.AppendLine();
        h.AppendLine("#include <algorithm>");
        h.AppendLine("#include <array>");
        h.AppendLine("#include <cstddef>");
        h.AppendLine("#include <cstdint>");
        h.AppendLine("#include <string_view>");
        h.AppendLine();
        h.AppendLine("// Generated by tools/questscan.ps1 -DisabledParents from the game's own files: do not edit by hand. The enable parents");
        h.AppendLine("// (references another reference names as its enable parent, XESP) that a script of the game's own turns off");
        h.AppendLine("// somewhere: a quest stage's fragment, a quest's, an alias's, a reference's, a base object's, a topic's, a scene's,");
        h.AppendLine("// a package's, a perk's or a terminal's script (tools/questscan.cs, ParentScan, says how it reads them). A marker its");
        h.AppendLine("// plugin has off that a quest turns on at one stage and off at a later one (BoSAirportGuards, 0019C046: BoSEnable's");
        h.AppendLine("// stage 10 on, its stage 200 and Inst307Fight's stage 10 off) is off in a game whose story is past it, and stays off");
        h.AppendLine("// there whatever a friend's game runs (EnableChain::Verdict::kStoryTurnsOff). Gun Run's marker (000C3D6F,");
        h.AppendLine("// MirelurkQuestEnabler) is only ever turned on, so it is not here.");
        h.AppendLine("// Scanned: " + string.Join(", ", scanned.ToArray()) + " (the add-ons not installed where it ran are not).");
        h.AppendLine(string.Format("// Of their {0} enable parents, {1} are here, {2} of them off in their plugin.", children.Count, table.Count, offInPlugin));
        h.AppendLine("namespace DisabledParents");
        h.AppendLine("{");
        h.AppendLine("\t// The game's own plugins, as Official.h names them, in their usual load order; an entry's file is its index here.");
        h.AppendLine("\tinline constexpr std::array<std::string_view, " + FILES.Length + "> FILES{ " + string.Join(", ", FILES.Select(f => "\"" + f + "\"").ToArray()) + " };");
        h.AppendLine();
        h.AppendLine("\tstruct Entry");
        h.AppendLine("\t{");
        h.AppendLine("\t\tstd::uint8_t  file;  // FILES");
        h.AppendLine("\t\tstd::uint32_t id;    // the form id in its file, without a load order's index");
        h.AppendLine("\t};");
        h.AppendLine();
        h.AppendLine("\t// Sorted by file and id. Each: its editor id, whether its plugin has it off, how many references name it as their");
        h.AppendLine("\t// parent, and the first script found that turns it off.");
        h.AppendLine("\tinline constexpr Entry PARENTS[] = {");
        foreach (var id in table)
        {
            Ref r; refs.TryGetValue(id, out r);
            var edid = r != null && r.Edid.Length > 0 ? r.Edid : "(no editor id)";
            h.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6} }},  // {2}: {3}, {4} children ({7} actors); {5}{6}", id >> 24, id & 0xFFFFFF, edid, r != null && (r.Flags & 0x800) != 0 ? "off" : "on", children[id], why[id][0], why[id].Count > 1 ? " (+" + (why[id].Count - 1) + " more)" : "", actors.ContainsKey(id) ? actors[id] : 0));
        }
        h.AppendLine("\t};");
        h.AppendLine();
        h.AppendLine("\t// How many entries the scan wrote (tests/EnableChainTests.cpp checks PARENTS against it: an entry added or lost by hand shows).");
        h.AppendLine("\tinline constexpr std::size_t SCANNED = " + table.Count + ";");
        h.AppendLine();
        h.AppendLine("\t// The index in FILES of a plugin's file name (any case), or FILES.size().");
        h.AppendLine("\tconstexpr std::size_t FileIndex(std::string_view a_file)");
        h.AppendLine("\t{");
        h.AppendLine("\t\tconst auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };");
        h.AppendLine("\t\tfor (std::size_t i = 0; i < FILES.size(); ++i) {");
        h.AppendLine("\t\t\tif (std::ranges::equal(FILES[i], a_file, [&](char a, char b) { return lower(a) == lower(b); })) {");
        h.AppendLine("\t\t\t\treturn i;");
        h.AppendLine("\t\t\t}");
        h.AppendLine("\t\t}");
        h.AppendLine("\t\treturn FILES.size();");
        h.AppendLine("\t}");
        h.AppendLine();
        h.AppendLine("\t// Whether a script of the game's own turns off the reference a_id (its form id; its load order's index is ignored)");
        h.AppendLine("\t// that the plugin a_file made.");
        h.AppendLine("\tconstexpr bool Contains(std::string_view a_file, std::uint32_t a_id)");
        h.AppendLine("\t{");
        h.AppendLine("\t\tconst auto file = FileIndex(a_file);");
        h.AppendLine("\t\tif (file == FILES.size()) {");
        h.AppendLine("\t\t\treturn false;");
        h.AppendLine("\t\t}");
        h.AppendLine("\t\tconst Entry key{ static_cast<std::uint8_t>(file), a_id & 0x00FFFFFF };");
        h.AppendLine("\t\tconst auto  less = [](const Entry& a, const Entry& b) { return a.file != b.file ? a.file < b.file : a.id < b.id; };");
        h.AppendLine("\t\treturn std::ranges::binary_search(PARENTS, key, less);");
        h.AppendLine("\t}");
        h.AppendLine("}");
        return new Result { Header = h.ToString().Replace("\r\n", "\n").Replace("\n", "\r\n"), Report = report.ToString(), Count = table.Count };
    }
}
