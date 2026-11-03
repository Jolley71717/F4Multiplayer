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
    // The game's masters the scans read, each with the archive of its scripts. A master's index here is its
    // src/game/StageRules.h File (the order the tables were first made in, not the game's load order).
    public class Master { public string Esm; public string Ba2; }
    public static readonly Master[] MASTERS = {
        new Master { Esm = "Fallout4.esm", Ba2 = "Fallout4 - Misc.ba2" },
        new Master { Esm = "DLCCoast.esm", Ba2 = "DLCCoast - Main.ba2" },
        new Master { Esm = "DLCRobot.esm", Ba2 = "DLCRobot - Main.ba2" },
        new Master { Esm = "DLCNukaWorld.esm", Ba2 = "DLCNukaWorld - Main.ba2" },
    };

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
    public class Quest
    {
        public uint Id; public string Edid = ""; public int Type = -1; public int Flags; public string Full = ""; public int Stages; public int Objectives; public List<int> Startup = new List<int>();
        public string Event = "";  // the story manager event that starts it (ENAM), if any
        public List<string> Fills = new List<string>();  // each alias's fill, name=forced, unique, external, created, event, fromalias, matching or empty
        public List<Alias> Aliases = new List<Alias>();  // the same, with each alias's ID and the alias it is filled from
    }

    // An alias: its ID (references and locations share one numbering), name, fill (Fill) and the alias it is made at or
    // found from (ALFA, ALNA, ALCA; -1 none).
    public class Alias { public int Id; public string Name = ""; public string Fill = ""; public int Source = -1; }

    // How an alias is filled, by the subrecords it has up to its ALED: forced to a reference or location, a unique
    // actor, another quest's alias, created at runtime, from the event that starts the quest, from another alias, or
    // found by its conditions ("matching": near another alias, or only conditions).
    static string Fill(HashSet<string> a_marks)
    {
        return a_marks.Contains("ALFR") || a_marks.Contains("ALFL") ? "forced" : a_marks.Contains("ALUA") ? "unique" :
            a_marks.Contains("ALEQ") ? "external" : a_marks.Contains("ALCO") ? "created" : a_marks.Contains("ALFE") ? "event" :
            a_marks.Contains("ALFA") || a_marks.Contains("ALRT") ? "fromalias" : a_marks.Contains("ALNA") || a_marks.Contains("CTDA") ? "matching" : "empty";
    }

    public static int Masters(string path) { return Masters(File.ReadAllBytes(path)); }

    static int Masters(byte[] a_all)
    {
        uint hsize = BitConverter.ToUInt32(a_all, 4);
        int masters = 0;
        for (int s = 24, se = 24 + (int)hsize; s < se; s += 6 + BitConverter.ToUInt16(a_all, s + 4))
            if (Encoding.ASCII.GetString(a_all, s, 4) == "MAST") masters++;
        return masters;
    }

    // The quests the file's story manager quest nodes start (SMQN NNAM), as the file's form IDs (a quest of the file's
    // own has its count of masters as its load index, one of Fallout4.esm's 00).
    public static HashSet<uint> StoryManagerQuests(string path)
    {
        var all = File.ReadAllBytes(path);
        var result = new HashSet<uint>();
        int p = 24 + (int)BitConverter.ToUInt32(all, 4);
        while (p < all.Length)
        {
            string rt = Encoding.ASCII.GetString(all, p, 4);
            uint rsize = BitConverter.ToUInt32(all, p + 4);
            if (rt == "GRUP") { p += 24; continue; }
            if (rt == "SMQN")
            {
                uint flags = BitConverter.ToUInt32(all, p + 8);
                byte[] body; int bo, bl;
                if ((flags & 0x40000) != 0) { int dsize = (int)BitConverter.ToUInt32(all, p + 24); body = Inflate(all, p + 28, (int)rsize - 4, dsize); bo = 0; bl = dsize; }
                else { body = all; bo = p + 24; bl = (int)rsize; }
                for (int s = bo, se = bo + bl; s < se; s += 6 + BitConverter.ToUInt16(body, s + 4))
                {
                    if (Encoding.ASCII.GetString(body, s, 4) == "NNAM" && BitConverter.ToUInt16(body, s + 4) >= 4) result.Add(BitConverter.ToUInt32(body, s + 6));
                }
            }
            p += 24 + (int)rsize;
        }
        return result;
    }

    public static List<Quest> Quests(string path, int fileIndex)
    {
        var all = File.ReadAllBytes(path);
        var list = new List<Quest>();
        // TES4 header record. A file's own records have its count of masters as their load index; the others are its
        // changes to its masters' quests, left to the master's own scan.
        int pos = 0;
        uint hsize = BitConverter.ToUInt32(all, 4);
        int masters = Masters(all);
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
                    if (rt == "QUST" && (id >> 24) == masters)
                    {
                        var q = new Quest();
                        q.Id = (id & 0x00FFFFFF) | ((uint)fileIndex << 24);
                        int s = bo; int se = bo + bl; int big = -1;
                        HashSet<string> alias = null; string aliasName = ""; int aliasId = -1, aliasSource = -1;
                        while (s < se)
                        {
                            string st = Encoding.ASCII.GetString(body, s, 4);
                            int ss = BitConverter.ToUInt16(body, s + 4);
                            if (big >= 0) { ss = big; big = -1; }
                            if (st == "XXXX") { big = (int)BitConverter.ToUInt32(body, s + 6); s += 6 + 4; continue; }
                            int d = s + 6;
                            if (alias == null)
                            {
                                if (st == "EDID") q.Edid = Encoding.ASCII.GetString(body, d, Math.Max(0, ss - 1));
                                if (st == "DNAM" && ss >= 12) { q.Flags = BitConverter.ToUInt16(body, d); q.Type = body[d + 8]; }
                                if (st == "ENAM" && ss >= 4) q.Event = Encoding.ASCII.GetString(body, d, 4);
                                if (st == "INDX") { q.Stages++; if (ss >= 3 && (body[d + 2] & 0x02) != 0) q.Startup.Add(BitConverter.ToUInt16(body, d)); }
                                if (st == "QOBJ") q.Objectives++;
                            }
                            // the aliases come last: each from its ALST, ALLS or ALCS to its ALED
                            if (st == "ALST" || st == "ALLS" || st == "ALCS") { alias = new HashSet<string>(); aliasName = ""; aliasId = ss >= 4 ? BitConverter.ToInt32(body, d) : -1; aliasSource = -1; }
                            else if (alias != null)
                            {
                                if (st == "ALID") aliasName = Encoding.ASCII.GetString(body, d, Math.Max(0, ss - 1));
                                if ((st == "ALFA" || st == "ALNA") && ss >= 4) aliasSource = BitConverter.ToInt32(body, d);
                                if (st == "ALCA" && ss >= 2) aliasSource = BitConverter.ToInt16(body, d);
                                alias.Add(st);
                                if (st == "ALED")
                                {
                                    var fill = Fill(alias);
                                    q.Fills.Add(aliasName + "=" + fill);
                                    q.Aliases.Add(new Alias { Id = aliasId, Name = aliasName, Fill = fill, Source = aliasSource });
                                    alias = null;
                                }
                            }
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

    // The quests src/game/QuestFilter.h's SAFE holds, by (file, form ID in it), once SafeScan.Load has made them: shared
    // with the host's sQuests = safe, so the other scans take them for shared too. Null: not made (the scans see only
    // what every session shares).
    public static HashSet<Tuple<int, uint>> Safe;

    public static string Shared(GearScan.Quest q, int a_file)
    {
        if (Safe != null && Safe.Contains(Tuple.Create(a_file, q.Id & 0xFFFFFF))) return "shared (safe)";
        if (q.Type > 0 && q.Type != 6) return "shared";
        if (q.Type == 0 && a_file == 0 && FINALES.Contains(q.Id & 0xFFFFFF)) return "shared (finale)";
        if (q.Type == 0 && q.Objectives == 0 && IsDialogueName(q.Edid)) return "shared (dialogue)";
        return "not shared";
    }

    // What a fragment takes from the player: each call, the item (the property it was read from, or the temp an alias's
    // reference or another call put it in: a quest item) and " xN" for RemoveItem's count.
    public static List<Tuple<string, string, string>> Takes(GearScan.Func f)
    {
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var hits = new List<Tuple<string, string, string>>();
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
            hits.Add(Tuple.Create(i.Args[0], i.Args[0].Equals("RemoveAllItems", StringComparison.OrdinalIgnoreCase) || i.Args[0].Equals("UnequipAll", StringComparison.OrdinalIgnoreCase) ? "" : source.Replace("::", "").Replace("_var", ""),
                i.Args.Count > 4 && i.Args[0].Equals("RemoveItem", StringComparison.OrdinalIgnoreCase) ? " x" + i.Args[4] : ""));
        }
        return hits;
    }

    public static string Run(string a_data)
    {
        var sb = new StringBuilder();
        var files = GearScan.MASTERS;
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
                    var hits = Takes(f).Select(t => t.Item1 + "(" + t.Item2 + t.Item3 + ")").ToList();
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
        var files = GearScan.MASTERS;
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

    // A walk through what a function or a stage runs: the function's own code (the instructions it can reach, see
    // Reachable), the script functions it calls (its quest's own script through kmyQuest or self, its parent's, any
    // script a property or local is typed with, with the literal arguments it passes) and the stages of the quests it
    // sets or starts (its own by SetStage on self, any other matched by the property's name, with that quest's start-up
    // stages), each as deep as it goes until a cycle. Direct says what a function's own code does that is looked for
    // (null: nothing); Target what setting a stage of another quest, or starting it, does in itself (null: nothing, its
    // stages are followed); Follow whether another quest's stages are followed at all (by its file). -Moves and
    // SafeScan's rule walk the same way.
    public class Walk
    {
        public Func<List<GearScan.Instr>, string> Direct;
        public Func<GearScan.Quest, string> Target = q => null;
        public Func<GearScan.Quest, int, bool> Follow = (q, file) => true;
        readonly Dictionary<string, string> funcs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> stages = new Dictionary<string, string>();

        // What a function does that is looked for, itself or through what it calls and sets, or null: a chain of what it
        // calls and sets, ending in Direct's or Target's answer. a_quest: the quest whose script or fragment it is (what
        // its SetStage on self sets), when known.
        public string Function(GearScan.Func f, string[] a_args, Tuple<GearScan.Quest, int> a_quest, HashSet<string> a_seen)
        {
            var key = f.Obj + "." + f.State + "." + f.Name + "(" + (a_args == null ? "" : string.Join(",", a_args.Select(a => a ?? "?").ToArray())) + ")@" + (a_quest == null ? "" : a_quest.Item2 + ":" + a_quest.Item1.Id);
            string cached;
            if (funcs.TryGetValue(key, out cached)) return cached;
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
                    var itself = own ? null : Target(target.Item1);
                    if (itself != null) { why = f.Obj + "." + f.Name + ": " + itself; break; }
                    if (!own && !Follow(target.Item1, target.Item2)) continue;
                    var list = new List<int>();
                    int s;
                    if (Is(i.Args[0], "SetStage") && i.Args.Count > 3 && int.TryParse(i.Args[3], out s)) list.Add(s);
                    if (!own) list.AddRange(target.Item1.Startup);  // starts it, if it isn't running
                    foreach (var st in list)
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
                var w2 = Function(g, args, q, a_seen);
                if (w2 != null) why = owner + "." + name + " -> " + w2;
            }
            a_seen.Remove(key);
            // only finds are kept: a none found while a caller up the chain was still open may have missed a cycle through it
            if (why != null) funcs[key] = why;
            return why;
        }

        public string Stage(int a_file, GearScan.Quest a_quest, int a_stage, HashSet<string> a_seen)
        {
            var key = a_file + ":" + a_quest.Id + ":" + a_stage;
            string cached;
            if (stages.TryGetValue(key, out cached)) return cached;
            if (!a_seen.Add(key)) return null;
            Dictionary<int, List<GearScan.Func>> all;
            List<GearScan.Func> list;
            string why = null;
            if (fragments.TryGetValue(Tuple.Create(a_file, a_quest.Id & 0xFFFFFF), out all) && all.TryGetValue(a_stage, out list))
                foreach (var f in list) { why = Function(f, null, Tuple.Create(a_quest, a_file), a_seen); if (why != null) break; }
            a_seen.Remove(key);
            if (why != null) stages[key] = why;
            return why;
        }

        // What a stage of a quest does that is looked for (the first found), or null.
        public string Stage(int a_file, GearScan.Quest a_quest, int a_stage) { return Stage(a_file, a_quest, a_stage, new HashSet<string>()); }
    }

    // How a function or stage moves the player, itself or through what it calls and sets.
    static readonly Walk moves = new Walk { Direct = Direct };
    static string Stage(int a_file, GearScan.Quest a_quest, int a_stage, HashSet<string> a_seen) { return moves.Stage(a_file, a_quest, a_stage, a_seen); }

    // every master's quests by form ID (load index masked off), by file
    public static readonly List<Dictionary<uint, GearScan.Quest>> perFile = new List<Dictionary<uint, GearScan.Quest>>();

    // How a stage of a quest moves the player (the first way found), or null.
    public static string StageMoves(int a_file, GearScan.Quest a_quest, int a_stage) { return Stage(a_file, a_quest, a_stage, new HashSet<string>()); }

    // A quest's stage fragments, by stage (none: an empty one).
    public static Dictionary<int, List<GearScan.Func>> Fragments(int a_file, uint a_quest)
    {
        Dictionary<int, List<GearScan.Func>> stages;
        return fragments.TryGetValue(Tuple.Create(a_file, a_quest & 0xFFFFFF), out stages) ? stages : new Dictionary<int, List<GearScan.Func>>();
    }

    // Reads every master's quests and scripts (once).
    public static void Load(string data)
    {
        if (perFile.Count > 0) return;
        var files = GearScan.MASTERS;
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
    }

    public static string Run(string data)
    {
        var sb = new StringBuilder();
        Load(data);
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

// The rows of src/game/StageRules.h's RULES for the shared quests of the masters asked for (their File index): each
// stage whose fragment moves the player (MoveScan, the same as -Moves), takes items from them (QuestScan: an item read
// from a property; one put in a temp by an alias's GetRef or another call is the quest's own item, left out) or waits,
// with the tenths of a second its fragment waits: its literal Utility.Wait arguments and 2 s for each MoveTo of the
// player in it. A take is flagged kPays, but whether its item is an ordinary one (caps, armor) or the quest's own (MS11's
// cables, read from a property too) is checked by hand: a quest item stays unflagged. A stage 0 that isn't its quest's
// start-up stage is left out (listed at the end): a developer's shortcut, never set in a game played through, by which
// StageRules::Starting judges every stage that starts the quest (as MQ203's and MQ204's 0). Bridges (kBridge) and the
// story (kStory) are not decided here. The Automatron and Nuka-World rows were made with it; the older rows of
// Fallout4.esm and DLCCoast.esm were made before it, their payments traced by hand, and differ from its output in their
// waits.
public static class RuleScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }

    static double Waits(GearScan.Func f)
    {
        double seconds = 0;
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var i in f.Code)
        {
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(i.Args[1], "GetPlayer")) { players.Add(i.Args[2]); continue; }
            double x;
            if (i.Op == 25 && Is(i.Args[0], "utility") && Is(i.Args[1], "Wait") && i.Args.Count > 3 && double.TryParse(i.Args[3], out x)) seconds += x;
            if (i.Op == 13 || i.Op == 14) { if (players.Contains(i.Args[1])) players.Add(i.Args[0]); else players.Remove(i.Args[0]); }
            if (i.Op == 23 && (Is(i.Args[0], "MoveTo") || Is(i.Args[0], "MoveToNode")) && (players.Contains(i.Args[1]) || i.Args[1].IndexOf("PlayerRef", StringComparison.OrdinalIgnoreCase) >= 0)) seconds += 2;
            // a call's result written over a variable that held the player
            if (i.Op == 23 || i.Op == 25) players.Remove(i.Args[2]);
            if (i.Op == 24) players.Remove(i.Args[1]);
        }
        return seconds;
    }

    public static string Run(string data, int[] a_files) { return Run(data, a_files, false); }

    // a_safeOnly: the rows of src/game/QuestFilter.h's SAFE quests alone (SafeScan.Load first).
    public static string Run(string data, int[] a_files, bool a_safeOnly)
    {
        MoveScan.Load(data);
        var sb = new StringBuilder();
        var left = new List<string>();
        foreach (var file in a_files)
        {
            foreach (var quest in MoveScan.perFile[file].Values.OrderBy(q => q.Id & 0xFFFFFF))
            {
                var shared = QuestScan.Shared(quest, file);
                if (shared == "not shared" || (a_safeOnly && shared != "shared (safe)")) continue;
                bool first = true;
                var stages = MoveScan.Fragments(file, quest.Id);
                foreach (var stage in stages.Keys.OrderBy(k => k))
                {
                    var moves = MoveScan.StageMoves(file, quest, stage);
                    var takes = stages[stage].SelectMany(f => QuestScan.Takes(f)).Where(t => t.Item2.Length > 0 && !t.Item2.StartsWith("temp", StringComparison.OrdinalIgnoreCase)).ToList();
                    var wait = (int)Math.Min(255, Math.Round(10 * stages[stage].Sum(f => Waits(f))));
                    if (moves == null && takes.Count == 0 && wait == 0) continue;
                    if (stage == 0 && !quest.Startup.Contains(0))
                    {
                        left.Add(string.Format("#   {0} {1:X6} 0 {2}: {3}", file, quest.Id & 0xFFFFFF, quest.Edid, moves ?? "waits " + wait));
                        continue;
                    }
                    var flags = new List<string>();
                    if (moves != null) flags.Add("kMoves");
                    if (takes.Count > 0) flags.Add("kPays");
                    var what = new List<string>();
                    if (moves != null)
                    {
                        var hops = moves.Split(new[] { " -> " }, StringSplitOptions.None);
                        var effect = hops[hops.Length - 1];
                        effect = effect.Substring(effect.LastIndexOf(": ", StringComparison.Ordinal) + 2);
                        what.Add(hops.Length > 1 ? effect + " (by " + hops[0] + ")" : effect);
                    }
                    if (takes.Count > 0) what.Add(string.Join(" ", takes.Select(t => t.Item1 + "(" + t.Item2 + t.Item3 + ")").Distinct().ToArray()));
                    var comment = (first ? quest.Edid + (what.Count > 0 ? ": " : "") : "") + string.Join("; ", what.ToArray());
                    sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, {4} }},{5}", file, quest.Id & 0xFFFFFF, stage, flags.Count > 0 ? string.Join(" | ", flags.ToArray()) : "0", wait,
                        comment.Length > 0 ? "  // " + comment : ""));
                    first = false;
                }
            }
        }
        if (left.Count > 0)
        {
            sb.AppendLine("# left out, a stage 0 that is not its quest's start-up stage:");
            foreach (var l in left) sb.AppendLine(l);
        }
        return sb.ToString();
    }
}

// The quests shared by their type (QuestFilter: side quests, the add-ons') that each game fills its own way, what
// src/game/QuestFilter.h's PER_GAME holds: the story manager starts them (an event of the quest's own, ENAM, or a
// story manager quest node, SMQN NNAM) and at least one of their aliases is filled by its conditions or from the
// event, so each game's run has its own targets, as a radiant's (src/game/RadiantRules.h). A friend's stage started
// one here with this game's own targets. Only those with objectives (the others are never reported), and not those
// RadiantRules.h names (its editor IDs, read from the file passed): those are decided there.
public static class PerGameScan
{
    public static string Run(string data, string a_radiantRules)
    {
        var names = System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(a_radiantRules), "\"(\\w+)\"");
        var listed = new HashSet<string>(names.Cast<System.Text.RegularExpressions.Match>().Select(m => m.Groups[1].Value), StringComparer.OrdinalIgnoreCase);
        var files = GearScan.MASTERS;
        var nodes = new List<HashSet<uint>>();
        var masters = new List<int>();
        for (int f = 0; f < files.Length; f++)
        {
            nodes.Add(GearScan.StoryManagerQuests(Path.Combine(data, files[f].Esm)));
            masters.Add(GearScan.Masters(Path.Combine(data, files[f].Esm)));
        }
        var sb = new StringBuilder();
        int radiants = 0;
        for (int file = 0; file < files.Length; file++)
        {
            foreach (var q in GearScan.Quests(Path.Combine(data, files[file].Esm), file).OrderBy(x => x.Id & 0xFFFFFF))
            {
                if (QuestScan.Shared(q, file) != "shared" || q.Objectives == 0) continue;
                uint local = q.Id & 0xFFFFFF;
                // a node of the quest's own file, or for one of Fallout4.esm's, of any file
                bool node = nodes[file].Contains(((uint)masters[file] << 24) | local) || (file == 0 && nodes.Any(n => n.Contains(local)));
                if (q.Event.Length == 0 && !node) continue;
                Func<string, int> count = kind => q.Fills.Count(x => x.EndsWith("=" + kind));
                if (count("matching") + count("event") == 0) continue;
                if (listed.Contains(q.Edid)) { radiants++; continue; }
                var how = new List<string>();
                if (count("matching") > 0) how.Add(count("matching") + " by conditions");
                if (count("event") > 0) how.Add(count("event") + " from the event");
                if (count("created") > 0) how.Add(count("created") + " made at runtime");
                sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6} }},  // {2}: {3}; aliases {4}", file, local, q.Edid, q.Event.Length > 0 ? "event " + q.Event : "a quest node", string.Join(", ", how.ToArray())));
            }
        }
        sb.AppendLine("# and " + radiants + " radiants src/game/RadiantRules.h names");
        return sb.ToString();
    }
}

// The quests src/game/QuestFilter.h's SAFE holds: those no session shares by their type (QuestFilter: a misc quest, or
// one under no type that is no dialogue quest or finale) that a friend's stage can be set on in another game as it is,
// by a rule read off the game's files. A quest of the game's own masters with objectives that is no radiant
// (src/game/RadiantRules.h) and no side quest each game fills its own way (QuestFilter's PER_GAME); no companion's
// (COM*, Followers, *Affinity*, *Companion*, CA_, CIS_: a companion's talks and affinity are each player's); that runs
// once or is started with the game (not repeatable); whose every alias is fixed: a forced reference or location, a
// unique actor, another quest's alias, one its scripts fill, or one made at or found from such an alias, none filled
// from the story manager's event or by its conditions (directly or through the alias it is made at or found from), so
// it holds the same in every game; and none of whose stages, walked as -Moves walks them (MoveScan.Walk: its fragment,
// the script functions it calls, the stages it sets or starts of the quests no session shares by its type; a shared
// quest's stage reaches a friend's game as the story's anyway), touches what each game keeps for itself (Direct): a
// companion's affinity, who is or may be a companion (or a stage of a companion's quest), a faction's hostility to
// another or to the player, a settlement's owner, or the player's whole inventory. Less QuestFilter.h's DENIED (read
// from the file passed): quests the rule passes that are each player's all the same (survival's, Nuka-World's park
// bookkeeping and outpost count). The quests the rule passes but for their objectives (a location's quest, DN035) are
// counted at the end: a quest with no objective counts as started here only once something shows it, which needs a
// rule of its own. Then, for each quest left out for a stage, the first stage found and its chain.
public static class SafeScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }

    static readonly System.Text.RegularExpressions.Regex COMPANION = new System.Text.RegularExpressions.Regex("^(COM|Followers|CA_|CIS_|.*Affinity|.*Companion)");

    // The (file, form ID) rows of a table in a header: from "a_name[] = {" to its "};".
    static HashSet<Tuple<int, uint>> Table(string a_header, string a_name)
    {
        var text = File.ReadAllText(a_header);
        int at = text.IndexOf(a_name + "[] = {", StringComparison.Ordinal);
        if (at < 0) throw new Exception(a_name + " not found in " + a_header);
        var body = text.Substring(at, text.IndexOf("};", at, StringComparison.Ordinal) - at);
        var rows = new HashSet<Tuple<int, uint>>();
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(body, @"\{\s*(\d+),\s*0x([0-9A-Fa-f]+)\s*\}"))
            rows.Add(Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16)));
        return rows;
    }

    // Whether an alias holds what each game picks itself: filled from the event or by its conditions, or made at or found
    // from an alias that is.
    static bool PerGame(Dictionary<int, GearScan.Alias> a_aliases, int a_id, int a_depth)
    {
        GearScan.Alias alias;
        if (a_depth > 20 || !a_aliases.TryGetValue(a_id, out alias)) return false;
        if (alias.Fill == "event" || alias.Fill == "matching") return true;
        return (alias.Fill == "fromalias" || alias.Fill == "created") && alias.Source >= 0 && PerGame(a_aliases, alias.Source, a_depth + 1);
    }

    static readonly HashSet<string> COMPANIONS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "SetCompanion", "DismissCompanion", "AddToPlayerFaction", "SetAvailableToBeCompanion", "SetPlayerTeammate", "AllowCompanion",
        "DisallowCompanion", "ClearCompanion", "ReleaseCompanion", "SetDogmeatCompanion", "DismissDogmeatCompanion", "ClearDogmeatCompanion" };
    static readonly HashSet<string> FACTIONS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "SetEnemy", "SetAlly", "SetReaction", "SetPlayerEnemy" };
    static readonly HashSet<string> SETTLEMENTS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "SetOwnedByPlayer" };

    // The kinds of what a stage does that make its quest each player's, as Why names them.
    static readonly Dictionary<string, string> KINDS = new Dictionary<string, string> {
        { "affinity", "touches affinity" }, { "companion", "changes companions" }, { "empties", "empties the inventory" },
        { "factions", "changes factions' hostility" }, { "settlement", "changes a settlement's owner" } };

    // What a function's own code does that makes its quest each player's, as "kind what" (a key of KINDS), or null: a
    // companion's affinity (an argument named *Affinity*, CA_*, or a call named *Affinity* that isn't a Get, Is or Trace);
    // companions (COMPANIONS: who is or can be one, a teammate, a dismissal); a faction's hostility to another or to the
    // player (FACTIONS, or adding to or taking out of a PlayerEnemy or PlayerAlly faction); a settlement's owner
    // (SetOwnedByPlayer); or emptying the player's inventory (RemoveAllItems, UnequipAll on the player).
    static string Direct(List<GearScan.Instr> a_code)
    {
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Func<string, bool> player = v => players.Contains(v) || Bare(v).IndexOf("PlayerRef", StringComparison.OrdinalIgnoreCase) >= 0;
        foreach (var i in a_code)
        {
            foreach (var a in i.Args)
            {
                var b = Bare(a);
                if (b.IndexOf("Affinity", StringComparison.OrdinalIgnoreCase) >= 0 || b.StartsWith("CA_", StringComparison.OrdinalIgnoreCase) || b.StartsWith("pCA_", StringComparison.OrdinalIgnoreCase)) return "affinity " + b;
            }
            if (i.Op != 23 && i.Op != 25 && i.Op != 13 && i.Op != 14) continue;
            if (i.Op == 13 || i.Op == 14) { if (player(i.Args[1])) players.Add(i.Args[0]); else players.Remove(i.Args[0]); continue; }
            var m = i.Op == 25 ? i.Args[1] : i.Args[0];
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(m, "GetPlayer")) { players.Add(i.Args[2]); continue; }
            if (COMPANIONS.Contains(m)) return "companion " + m;
            if (FACTIONS.Contains(m)) return "factions " + m;
            if (SETTLEMENTS.Contains(m)) return "settlement " + m;
            if (m.IndexOf("Affinity", StringComparison.OrdinalIgnoreCase) >= 0 && !m.StartsWith("Get", StringComparison.OrdinalIgnoreCase) && !m.StartsWith("Is", StringComparison.OrdinalIgnoreCase) && !m.StartsWith("Trace", StringComparison.OrdinalIgnoreCase)) return "affinity " + m;
            if (i.Op == 25) continue;
            if ((Is(m, "AddToFaction") || Is(m, "RemoveFromFaction") || Is(m, "SetFactionRank")) && i.Args.Skip(3).Any(a => Bare(a).IndexOf("PlayerEnemy", StringComparison.OrdinalIgnoreCase) >= 0 || Bare(a).IndexOf("PlayerAlly", StringComparison.OrdinalIgnoreCase) >= 0))
                return "factions " + m + " " + Bare(i.Args[3]);
            if ((Is(m, "GetReference") || Is(m, "GetRef") || Is(m, "GetActorRef") || Is(m, "GetActorReference")) && Bare(i.Args[1]).IndexOf("Player", StringComparison.OrdinalIgnoreCase) >= 0) { players.Add(i.Args[2]); continue; }
            if ((Is(m, "RemoveAllItems") || Is(m, "UnequipAll")) && player(i.Args[1])) return "empties " + m;
            players.Remove(i.Args[2]);
        }
        return null;
    }

    // Setting a stage of a companion's quest (its name as the rule's COMPANION), or starting one, is a companion's.
    static string Target(GearScan.Quest q)
    {
        return COMPANION.IsMatch(System.Text.RegularExpressions.Regex.Replace(q.Edid, @"^DLC\d+_?", "")) ? "companion SetStage " + q.Edid : null;
    }

    // What each stage does, followed into the scripts it calls and the stages it sets or starts as -Moves follows them,
    // but for the stages of a quest every session with the story shares by its type (the story, a side or faction quest,
    // a dialogue quest, a finale: QuestScan.Shared): what one of those does a friend's game is sent anyway, SAFE or not.
    static readonly MoveScan.Walk walk = new MoveScan.Walk { Direct = Direct, Target = Target, Follow = (q, file) => QuestScan.Shared(q, file) == "not shared" };

    // The first stage of a quest that makes it each player's, by quest (file, form ID): its stage and the chain found.
    static readonly Dictionary<Tuple<int, uint>, string> found = new Dictionary<Tuple<int, uint>, string>();

    // Why a quest is not in SAFE ("" it is), as the rule reads it in order; "no objectives" for one that passes all else.
    static string Why(GearScan.Quest q, int a_file, HashSet<uint> a_radiants, HashSet<Tuple<int, uint>> a_perGame)
    {
        uint local = q.Id & 0xFFFFFF;
        if (QuestScan.Shared(q, a_file) != "not shared") return "shared by its type";
        if (a_file == 0 && a_radiants.Contains(local)) return "a radiant (RadiantRules.h)";
        if (a_perGame.Contains(Tuple.Create(a_file, local))) return "each game's (PER_GAME)";
        var bare = System.Text.RegularExpressions.Regex.Replace(q.Edid, @"^DLC\d+_?", "");
        if (COMPANION.IsMatch(bare)) return "a companion's";
        if ((q.Flags & 0x101) == 0) return "repeatable";
        var aliases = new Dictionary<int, GearScan.Alias>();
        foreach (var a in q.Aliases) if (!aliases.ContainsKey(a.Id)) aliases[a.Id] = a;
        if (q.Aliases.Any(a => PerGame(aliases, a.Id, 0))) return "an alias each game fills";
        foreach (var stage in MoveScan.Fragments(a_file, q.Id).Keys.OrderBy(k => k))
        {
            var each = walk.Stage(a_file, q, stage);
            if (each == null) continue;
            found[Tuple.Create(a_file, local)] = stage + " " + each;
            var last = each.Substring(each.LastIndexOf(": ", StringComparison.Ordinal) + 2);
            return "a stage that " + KINDS[last.Substring(0, last.IndexOf(' '))];
        }
        if (q.Objectives == 0) return "no objectives";
        return "";
    }

    // Makes the SAFE set (QuestScan.Safe), less DENIED, and returns each quest's verdict, by file and form ID.
    public static SortedDictionary<Tuple<int, uint>, string> Load(string data, string a_questFilter, string a_radiantRules)
    {
        MoveScan.Load(data);
        QuestScan.Safe = null;
        var radiants = new HashSet<uint>(System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(a_radiantRules), "Entry\\{\\s*0x([0-9A-Fa-f]+),\\s*\"")
            .Cast<System.Text.RegularExpressions.Match>().Select(m => Convert.ToUInt32(m.Groups[1].Value, 16) & 0xFFFFFF));
        var perGame = Table(a_questFilter, "PER_GAME");
        var denied = Table(a_questFilter, "DENIED");
        var verdicts = new SortedDictionary<Tuple<int, uint>, string>();
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var q in MoveScan.perFile[file].Values)
            {
                var key = Tuple.Create(file, q.Id & 0xFFFFFF);
                var why = Why(q, file, radiants, perGame);
                verdicts[key] = why == "" && denied.Contains(key) ? "denied (DENIED)" : why;
            }
        QuestScan.Safe = new HashSet<Tuple<int, uint>>(verdicts.Where(kv => kv.Value == "").Select(kv => kv.Key));
        return verdicts;
    }

    // SAFE's rows, then what was left out and why.
    public static string Run(string data, string a_questFilter, string a_radiantRules)
    {
        var verdicts = Load(data, a_questFilter, a_radiantRules);
        var sb = new StringBuilder();
        var byFile = new int[GearScan.MASTERS.Length];
        int stages = 0;
        foreach (var kv in verdicts.Where(kv => kv.Value == ""))
        {
            var q = MoveScan.perFile[kv.Key.Item1][kv.Key.Item2];
            byFile[kv.Key.Item1]++;
            stages += q.Stages;
            sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6} }},  // {2}", kv.Key.Item1, kv.Key.Item2, q.Edid));
        }
        sb.AppendLine(string.Format("# {0} quests ({1}), {2} stages", QuestScan.Safe.Count, string.Join(", ", GearScan.MASTERS.Select((m, i) => m.Esm + " " + byFile[i]).ToArray()), stages));
        foreach (var kv in verdicts.Where(kv => kv.Value == "denied (DENIED)"))
            sb.AppendLine(string.Format("# denied: {0} 0x{1:X6} {2}", kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid));
        foreach (var g in verdicts.Values.Where(v => v != "" && !v.StartsWith("denied")).GroupBy(v => v).OrderBy(g => g.Key))
            sb.AppendLine(string.Format("# left out, {0}: {1}", g.Key, g.Count()));
        // what each quest left out for a stage does, and how its stage gets there, but the shared ones'
        foreach (var kv in found.Where(kv => verdicts[kv.Key].StartsWith("a stage")).OrderBy(kv => kv.Key))
            sb.AppendLine(string.Format("#   {0} 0x{1:X6} {2} {3}", kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid, kv.Value));
        return sb.ToString();
    }
}
