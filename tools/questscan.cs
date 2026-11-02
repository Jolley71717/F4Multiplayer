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
    static byte[] Inflate(byte[] data, int offset, int length, int size)
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
    public class Obj { public string Name; public string Parent = ""; public Dictionary<string, string> Types = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase); }

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
            int vc = r.U16(); for (int v = 0; v < vc; v++) { var vn = r.Str(); obj.Types[vn] = r.Str(); r.U32(); r.Var(); r.U8(); }
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
