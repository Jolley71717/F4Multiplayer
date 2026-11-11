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
    public class Quest
    {
        public uint Id; public string Edid = ""; public int Type = -1; public int Flags; public string Full = ""; public int Stages; public int Objectives; public List<int> Startup = new List<int>();
        public List<int> ObjectiveIds = new List<int>();  // each objective's index (QOBJ), in the record's order
        public string Event = "";  // the story manager event that starts it (ENAM), if any
        public List<string> Scripts = new List<string>();  // the scripts attached to the quest itself (its VMAD), not its stage fragments' or aliases'
        public List<string> Fills = new List<string>();  // each alias's fill, name=forced, unique, external, created, event, fromalias, matching or empty
        public List<Alias> Aliases = new List<Alias>();  // the same, with each alias's ID and the alias it is filled from
        // The object properties of every script in the quest's VMAD (its own scripts, its stage fragments' and its aliases'),
        // by name: their forms as MASTERS index << 24 | form ID (a form of a file not in MASTERS is left out; a property of
        // several scripts keeps the first with values). What a fragment's SetEnemy(DLC03ChildrenofAtomFaction) names.
        public Dictionary<string, List<uint>> Props = new Dictionary<string, List<uint>>(StringComparer.OrdinalIgnoreCase);
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

    static int Masters(byte[] a_all) { return MasterNames(a_all).Count; }

    // The file's masters (TES4 MAST), in its load order: a form ID's top byte below their count is one of them.
    static List<string> MasterNames(byte[] a_all)
    {
        uint hsize = BitConverter.ToUInt32(a_all, 4);
        var masters = new List<string>();
        for (int s = 24, se = 24 + (int)hsize; s < se; s += 6 + BitConverter.ToUInt16(a_all, s + 4))
            if (Encoding.ASCII.GetString(a_all, s, 4) == "MAST") masters.Add(Encoding.ASCII.GetString(a_all, s + 6, BitConverter.ToUInt16(a_all, s + 4) - 1));
        return masters;
    }

    // A form ID read in the file at MASTERS index a_file (its load order's index on top) as MASTERS index << 24 | form ID,
    // or 0xFFFFFFFF for a file not in MASTERS.
    public static uint Local(uint a_id, List<string> a_masters, int a_file)
    {
        int high = (int)(a_id >> 24);
        string file = high < a_masters.Count ? a_masters[high] : MASTERS[a_file].Esm;
        int index = Array.FindIndex(MASTERS, m => m.Esm.Equals(file, StringComparison.OrdinalIgnoreCase));
        return index < 0 ? 0xFFFFFFFF : (uint)index << 24 | (a_id & 0xFFFFFF);
    }

    // The file's own factions (FACT): editor ID -> MASTERS index << 24 | form ID.
    public static Dictionary<string, uint> Factions(string path, int fileIndex)
    {
        var all = File.ReadAllBytes(path);
        var result = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
        int masters = Masters(all);
        int pos = 24 + (int)BitConverter.ToUInt32(all, 4);
        while (pos < all.Length)
        {
            string type = Encoding.ASCII.GetString(all, pos, 4);
            uint gsize = BitConverter.ToUInt32(all, pos + 4);
            if (type != "GRUP" || Encoding.ASCII.GetString(all, pos + 8, 4) != "FACT") { pos += type == "GRUP" ? (int)gsize : 24 + (int)gsize; continue; }
            int end = pos + (int)gsize, p = pos + 24;
            while (p < end)
            {
                string rt = Encoding.ASCII.GetString(all, p, 4);
                uint rsize = BitConverter.ToUInt32(all, p + 4);
                if (rt == "GRUP") { p += 24; continue; }
                uint flags = BitConverter.ToUInt32(all, p + 8), id = BitConverter.ToUInt32(all, p + 12);
                byte[] body; int bo, bl;
                if ((flags & 0x40000) != 0) { int dsize = (int)BitConverter.ToUInt32(all, p + 24); body = Inflate(all, p + 28, (int)rsize - 4, dsize); bo = 0; bl = dsize; }
                else { body = all; bo = p + 24; bl = (int)rsize; }
                if (rt == "FACT" && (id >> 24) == masters)
                    for (int s = bo, se = bo + bl; s < se; s += 6 + BitConverter.ToUInt16(body, s + 4))
                        if (Encoding.ASCII.GetString(body, s, 4) == "EDID") { result[Encoding.ASCII.GetString(body, s + 6, Math.Max(0, BitConverter.ToUInt16(body, s + 4) - 1))] = (id & 0xFFFFFF) | ((uint)fileIndex << 24); break; }
                p += 24 + (int)rsize;
            }
            break;
        }
        return result;
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
        var names = MasterNames(all);
        int masters = names.Count;
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
                                if (st == "QOBJ") { q.Objectives++; if (ss >= 2) q.ObjectiveIds.Add(BitConverter.ToUInt16(body, d)); }
                                if (st == "VMAD") { q.Scripts = ParentScan.OwnScripts(body, d, ss); q.Props = ParentScan.QuestProperties(body, d, ss, names, fileIndex); }
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

    // The quests src/game/QuestFilter.h's SAFE holds, by (file, form ID in it), once SafeScan.Load has made them: shared
    // with the host's sQuests = safe, so the other scans take them for shared too. Null: not made (the scans see only
    // what every session shares).
    public static HashSet<Tuple<int, uint>> Safe;

    // The quests QuestFilter.h's SILENT holds, once SafeScan.Load has made them: those the rule passes but for their
    // objectives, less the ones denied by name or by hand. Shared with sQuests = safe too, stages unordered as a
    // dialogue quest's. Null: not made.
    public static HashSet<Tuple<int, uint>> Silent;

    // The radiants (src/game/RadiantRules.h, by file and form ID in it) and QuestFilter.h's PER_GAME, once
    // SafeScan.Load has read them (null: not read). Quests shared by their type that no friend's stage is ever set on.
    public static HashSet<Tuple<int, uint>> Radiants;
    public static HashSet<Tuple<int, uint>> PerGame;

    // Whether a stage set in one game is set on this quest in a friend's: shared by its type, finale or dialogue quest
    // (Shared), but not a radiant (RadiantRules.h decides those, by their run) and not PER_GAME (never shared). A quest
    // that does not reach a friend is one whose stages a walk through a shared stage's fragment has to follow: the
    // friend's game runs that fragment, and what it sets there is set there only.
    public static bool ReachesFriend(GearScan.Quest q, int a_file)
    {
        if (Shared(q, a_file) == "not shared") return false;
        uint local = q.Id & 0xFFFFFF;
        if (Radiants != null && Radiants.Contains(Tuple.Create(a_file, local))) return false;
        if (PerGame != null && PerGame.Contains(Tuple.Create(a_file, local))) return false;
        return true;
    }

    public static string Shared(GearScan.Quest q, int a_file)
    {
        if (Safe != null && Safe.Contains(Tuple.Create(a_file, q.Id & 0xFFFFFF))) return "shared (safe)";
        if (Silent != null && Silent.Contains(Tuple.Create(a_file, q.Id & 0xFFFFFF))) return "shared (silent)";
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

    // The quest with this editor ID (and its file), or null.
    public static Tuple<GearScan.Quest, int> QuestByEdid(string a_edid)
    {
        Tuple<GearScan.Quest, int> t;
        return byEdid.TryGetValue(a_edid, out t) ? t : null;
    }

    // The quest a script is attached to (its own VMAD), by script name: a script on exactly one quest only. One attached to
    // several (a template) says nothing about which a variable typed with it holds.
    static readonly Dictionary<string, Tuple<GearScan.Quest, int>> questScripts = new Dictionary<string, Tuple<GearScan.Quest, int>>(StringComparer.OrdinalIgnoreCase);
    static readonly HashSet<string> sharedScripts = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

    static void NoteScripts(GearScan.Quest q, int a_file)
    {
        foreach (var name in q.Scripts.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (questScripts.ContainsKey(name)) { questScripts.Remove(name); sharedScripts.Add(name); }
            else if (!sharedScripts.Contains(name)) questScripts[name] = Tuple.Create(q, a_file);
        }
    }

    // The quest whose script a variable is typed with, when that script is on one quest only (null: not a quest's).
    static Tuple<GearScan.Quest, int> QuestOfScript(string a_type)
    {
        Tuple<GearScan.Quest, int> t;
        return a_type != null && questScripts.TryGetValue(a_type, out t) ? t : null;
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
    // (null: nothing); Target what setting a stage of another quest, or starting it, does in itself (by its file and the
    // call, SetStage or Start; null: nothing, its stages are followed; never asked of the quest's own, by self or by its
    // own property); Follow whether another quest's stages are followed at all (by its file). -Moves and SafeScan's rule
    // walk the same way.
    public class Walk
    {
        public Func<List<GearScan.Instr>, string> Direct;
        public Func<GearScan.Quest, int, string, string> Target = (q, file, call) => null;
        public Func<GearScan.Quest, int, bool> Follow = (q, file) => true;
        // Said of each stage a function sets or starts (file, quest, stage; its own quest's too) before it is walked: what
        // is wrong with the stage itself, or null (the walk goes on into it). Not said of the stage Stage was asked about.
        public Func<int, GearScan.Quest, int, string> Reaches;
        // Whether a script function called is walked into (by its name): SafeScan's walk of an ALLOW_OUTCOMES quest does not
        // go into what a settlement's own scripts do once it is given or taken (SetOwnedByPlayer) or gains or loses a settler.
        public Func<string, bool> Enter = name => true;
        // Whether a start or SetStage of another quest (by the quest whose script or fragment calls it, the quest started and the
        // call) is let through without asking Target: its stages are then walked as any (SafeScan.ALLOW_STARTS).
        public Func<Tuple<GearScan.Quest, int>, GearScan.Quest, string, bool> Allows = (from, to, call) => false;
        // Whether a start Allows lets through is left unwalked (the quest started, its file): a radiant shared by its run
        // (RadiantRules.h), whose stages reach a friend's game as that run's, not as the stage's that set one (SafeScan).
        public Func<GearScan.Quest, int, bool> Unwalked = (to, file) => false;
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
                    var allowed = !own && !(a_quest != null && target.Item1 == a_quest.Item1) && Allows(a_quest, target.Item1, i.Args[0]);
                    var itself = own || (a_quest != null && target.Item1 == a_quest.Item1) || allowed ? null : Target(target.Item1, target.Item2, i.Args[0]);
                    if (itself != null) { why = f.Obj + "." + f.Name + ": " + itself; break; }
                    if (!own && (!Follow(target.Item1, target.Item2) || (allowed && Unwalked(target.Item1, target.Item2)))) continue;
                    var list = new List<int>();
                    int s;
                    if (Is(i.Args[0], "SetStage") && i.Args.Count > 3 && int.TryParse(i.Args[3], out s)) list.Add(s);
                    if (!own) list.AddRange(target.Item1.Startup);  // starts it, if it isn't running
                    foreach (var st in list)
                    {
                        var w = Reaches == null ? null : Reaches(target.Item2, target.Item1, st);
                        if (w != null) { why = (own ? "" : target.Item1.Edid + " ") + st + " -> " + w; break; }
                        w = Stage(target.Item2, target.Item1, st, a_seen);
                        if (w != null) { why = (own ? "" : target.Item1.Edid + " ") + st + " -> " + w; break; }
                    }
                    continue;
                }
                string type, name;
                if (i.Op == 25) { type = i.Args[0]; name = i.Args[1]; }
                else if (i.Op == 24) { Script sc; type = scripts.TryGetValue(f.Obj, out sc) ? sc.Obj.Parent : null; name = i.Args[0]; }
                else { type = TypeOf(f, i.Args[1]); name = i.Args[0]; }
                if (type == null || !Enter(name)) continue;
                string owner;
                var g = Find(type, name, out owner);
                if (g == null) continue;
                // a call on the quest's own script keeps the quest (kmyQuest.StartQuest), and one through a variable typed with the
                // script of exactly one quest is that quest's (DN036_Post 15: DN036QuestScript.InstituteEnding sets DN036 stages),
                // any other script's doesn't
                var q = (i.Op == 23 && Own(i.Args[1])) || i.Op == 24 ? a_quest : i.Op == 23 ? QuestOfScript(type) : null;
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
            foreach (var q in GearScan.Quests(Path.Combine(data, files[file].Esm), file)) { d[q.Id & 0xFFFFFF] = q; if (!byEdid.ContainsKey(q.Edid)) byEdid[q.Edid] = Tuple.Create(q, file); NoteScripts(q, file); }
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

// The enable parents (a reference some placed reference names in its XESP) that a script of the game's own turns off
// somewhere: what src/game/DisabledParents.h holds (tools/questscan.ps1 -DisabledParents writes it). Read from the
// official plugins found (Fallout4.esm and the add-ons) and their script archives. A script turns a reference off when
// it calls Disable or DisableNoWait on it (TryToDisable or TryToDisableNoWait on an alias forced to it, DisableAll on a
// collection alias it fills, DisableLinkChain on what links to it): through a property that names it, an alias property
// whose alias is forced to it (or to an alias of another quest that is) or filled with it as a forced location's
// reference of a ref type (ALFA and ALRT: the location's LCSR and ACSR entries of that type, a collection alias every
// one of them), itself (a script attached to it, to its base or to an alias forced to or filled with it), its linked
// reference (by the keyword it is called with, where that is known), or by passing one of those to a function, a few
// calls deep, that does. A property is read from every script attached to a record (VMAD): a quest's own, its stage
// fragments' and its aliases', a reference's (with its base's), a base object's, a topic's, a scene's, a package's, a
// perk's and a terminal's. Each attachment is judged with the values its properties have there: a branch on a bool,
// number or string property that no script ever sets goes the one way those values send it (DefaultEnableDisableTrigScript
// with bEnableLinkedRef only enables), any other branch both ways. Over-counted on purpose: any function of a script,
// whether a game ever calls it or not. What it does not see: a reference reached through a variable set at run time, a
// struct's member, a collection element reached by index (GetAt), an alias filled by conditions or from a location that
// isn't forced, a console command. The table's header names the files it read (COVERED): a parent from another official
// file is never switched on.
public static class ParentScan
{
    // the game's own plugins in load order, as Official.h names them, and the archive that holds each one's scripts
    public static readonly string[] FILES = { "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm", "DLCNukaWorld.esm" };
    static string ScriptArchive(string a_esm) { return a_esm == "Fallout4.esm" ? "Fallout4 - Misc.ba2" : Path.GetFileNameWithoutExtension(a_esm) + " - Main.ba2"; }

    static readonly HashSet<string> PLACED = new HashSet<string> { "REFR", "ACHR", "PGRE", "PMIS", "PARW", "PBAR", "PBEA", "PCON", "PFLA", "PHZD" };
    static readonly HashSet<string> SKIP = new HashSet<string> { "LAND", "NAVM", "NAVI", "WRLD", "CELL", "IMAD", "LSCR" };
    static readonly HashSet<string> FRAGMENTS = new HashSet<string> { "QUST", "INFO", "PACK", "SCEN", "PERK", "TERM" };

    class Ref { public string Edid = ""; public uint Base; public uint Parent; public uint Flags; public bool Actor; public List<Tuple<uint, uint>> Links = new List<Tuple<uint, uint>>(); }  // links: keyword, reference
    class Prop { public string Name; public string Literal; public List<uint> Forms = new List<uint>(); public List<int> Aliases = new List<int>(); }
    class Attached { public string Script; public List<Prop> Props = new List<Prop>(); }
    // a script attached somewhere: what self is there, and where, for the table's comments
    class Attachment { public Attached Entry; public string Where; public uint Self; public uint SelfQuest; public int SelfAlias = -1; }

    static readonly Dictionary<uint, Ref> refs = new Dictionary<uint, Ref>();
    static readonly Dictionary<uint, Dictionary<int, uint>> forced = new Dictionary<uint, Dictionary<int, uint>>();      // quest -> alias -> forced reference
    static readonly Dictionary<uint, Dictionary<int, Tuple<uint, int>>> external = new Dictionary<uint, Dictionary<int, Tuple<uint, int>>>();  // quest -> alias -> another quest's alias
    // quest -> alias -> the location alias (ALFA) whose location's references of these ref types (ALRT) fill it
    static readonly Dictionary<uint, Dictionary<int, Tuple<int, List<uint>>>> byRefType = new Dictionary<uint, Dictionary<int, Tuple<int, List<uint>>>>();
    static readonly Dictionary<uint, Dictionary<int, uint>> forcedLocation = new Dictionary<uint, Dictionary<int, uint>>();  // quest -> location alias -> forced location (ALFL)
    static readonly Dictionary<uint, List<Tuple<uint, uint>>> locationRefs = new Dictionary<uint, List<Tuple<uint, uint>>>();  // location -> its LCSR and ACSR: ref type, reference
    const int LOCATION_REF = 16;  // the size of an LCSR or ACSR entry
    static int locationRefOdd;    // LCSR or ACSR subrecords not a whole number of entries
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

    // The names of the scripts attached to a record itself (the first part of its VMAD at a_at, a_size long): not a
    // quest's fragments' or aliases'. None if it can't be read.
    public static List<string> OwnScripts(byte[] a_body, int a_at, int a_size)
    {
        var names = new List<string>();
        try
        {
            var v = new V { B = a_body, P = a_at, End = a_at + a_size };
            v.S16(); v.Format = v.S16();
            foreach (var s in Scripts(v)) if (s != null) names.Add(s.Script);
        }
        catch (Exception) { names.Clear(); }
        return names;
    }

    // The object properties of every script in a quest's VMAD (its own, its stage fragments' and its aliases'), by name,
    // their forms as GearScan.Local gives them (a_masters: the file's masters, a_file: its MASTERS index). A property of
    // several scripts keeps the first with values. Empty if the VMAD can't be read.
    public static Dictionary<string, List<uint>> QuestProperties(byte[] a_body, int a_at, int a_size, List<string> a_masters, int a_file)
    {
        var props = new Dictionary<string, List<uint>>(StringComparer.OrdinalIgnoreCase);
        Action<Attached> add = s =>
        {
            if (s == null) return;
            foreach (var p in s.Props)
            {
                var ids = p.Forms.Select(f => GearScan.Local(f, a_masters, a_file)).Where(x => x != 0xFFFFFFFF).ToList();
                if (ids.Count > 0 && !props.ContainsKey(p.Name)) props[p.Name] = ids;
            }
        };
        try
        {
            var v = new V { B = a_body, P = a_at, End = a_at + a_size };
            v.S16(); v.Format = v.S16();
            foreach (var s in Scripts(v)) add(s);
            if (v.P < v.End)
            {
                v.U8();
                int frags = v.U16();
                add(ReadScript(v));
                for (int k = 0; k < frags; k++) { v.U16(); v.S16(); v.U32(); v.U8(); v.W(); v.W(); }
                int aliases = v.U16();
                for (int k = 0; k < aliases; k++)
                {
                    uint form; int alias; v.Obj(out form, out alias);
                    v.S16(); v.Format = v.S16();
                    foreach (var s in Scripts(v)) add(s);
                }
            }
        }
        catch (Exception) { }
        return props;
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
            int questAlias = -1, locationAlias = -1;
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
                else if (type == "QUST" && st == "ALST") { questAlias = (int)BitConverter.ToUInt32(body, d); locationAlias = -1; }
                else if (type == "QUST" && st == "ALLS") { questAlias = -1; locationAlias = (int)BitConverter.ToUInt32(body, d); }
                else if (type == "QUST" && st == "ALED") { questAlias = -1; locationAlias = -1; }
                else if (type == "QUST" && st == "ALFL" && locationAlias >= 0)
                {
                    if (!forcedLocation.ContainsKey(key)) forcedLocation[key] = new Dictionary<int, uint>();
                    forcedLocation[key][locationAlias] = Key(BitConverter.ToUInt32(body, d), masters, a_file);
                }
                else if (type == "QUST" && (st == "ALFA" || st == "ALRT") && questAlias >= 0)
                {
                    if (!byRefType.ContainsKey(key)) byRefType[key] = new Dictionary<int, Tuple<int, List<uint>>>();
                    Tuple<int, List<uint>> fill;
                    if (!byRefType[key].TryGetValue(questAlias, out fill)) byRefType[key][questAlias] = fill = Tuple.Create(-1, new List<uint>());
                    if (st == "ALFA") byRefType[key][questAlias] = Tuple.Create((int)BitConverter.ToUInt32(body, d), fill.Item2);
                    else fill.Item2.Add(Key(BitConverter.ToUInt32(body, d), masters, a_file));
                }
                else if (type == "LCTN" && (st == "LCSR" || st == "ACSR"))
                {
                    // each: ref type, reference, its cell or world, grid y and x (int16 each)
                    if (ss % LOCATION_REF != 0) locationRefOdd++;
                    List<Tuple<uint, uint>> list;
                    if (!locationRefs.TryGetValue(key, out list)) locationRefs[key] = list = new List<Tuple<uint, uint>>();
                    for (int k = 0; k + LOCATION_REF <= ss; k += LOCATION_REF)
                        list.Add(Tuple.Create(Key(BitConverter.ToUInt32(body, d + k), masters, a_file), Key(BitConverter.ToUInt32(body, d + k + 4), masters, a_file)));
                }
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

    static readonly HashSet<string> OFF = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "Disable", "DisableNoWait", "TryToDisable", "TryToDisableNoWait", "DisableAll" };
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

    // The references an alias is known to hold: the one it is forced to (ALFR, or another quest's alias it is: ALEQ and
    // ALEA), or a forced location's references of its ref types (ALFA, ALRT; a collection alias holds every one).
    static List<uint> Fills(uint a_quest, int a_alias, int a_depth)
    {
        var list = new List<uint>();
        Dictionary<int, uint> f; uint r;
        if (forced.TryGetValue(a_quest, out f) && f.TryGetValue(a_alias, out r)) { list.Add(r); return list; }
        Dictionary<int, Tuple<uint, int>> e; Tuple<uint, int> t;
        if (a_depth < 8 && external.TryGetValue(a_quest, out e) && e.TryGetValue(a_alias, out t) && t.Item2 >= 0) return Fills(t.Item1, t.Item2, a_depth + 1);
        Dictionary<int, Tuple<int, List<uint>>> b; Tuple<int, List<uint>> fill; Dictionary<int, uint> l; uint location; List<Tuple<uint, uint>> entries;
        if (byRefType.TryGetValue(a_quest, out b) && b.TryGetValue(a_alias, out fill) && fill.Item1 >= 0 && fill.Item2.Count > 0 &&
            forcedLocation.TryGetValue(a_quest, out l) && l.TryGetValue(fill.Item1, out location) && locationRefs.TryGetValue(location, out entries))
            foreach (var x in entries) if (fill.Item2.Contains(x.Item1) && !list.Contains(x.Item2)) list.Add(x.Item2);
        return list;
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
            var self = a.Self != 0 ? new List<uint> { a.Self } : a.SelfAlias >= 0 ? Fills(a.SelfQuest, a.SelfAlias, 0) : new List<uint>();
            var sink = Judge(a.Entry.Script, a.Entry.Props);
            judgedCount++;
            if (sink.Off.Count == 0) continue;
            var entry = a.Entry;
            // what an origin ("S", "P<name>") names here
            Func<string, List<uint>> named = o =>
            {
                if (o == "S") return self;
                var p = o.StartsWith("P") ? entry.Props.FirstOrDefault(x => Is(x.Name, o.Substring(1))) : null;
                if (p == null) return new List<uint>();
                return p.Forms.SelectMany((form, k) => p.Aliases[k] >= 0 ? Fills(form, p.Aliases[k], 0) : new List<uint> { form }).ToList();
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
        report.AppendLine(string.Format("# {0} location references of a ref type read ({1} subrecords not whole entries)", locationRefs.Values.Sum(x => x.Count), locationRefOdd));
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
        h.AppendLine("// Scanned (COVERED): " + string.Join(", ", scanned.ToArray()) + ". A parent from an official file it");
        h.AppendLine("// didn't read (an add-on not installed where it ran) is never switched on (EnableChain::Verdict::kNotCovered).");
        h.AppendLine(string.Format("// Of their {0} enable parents, {1} are here, {2} of them off in their plugin.", children.Count, table.Count, offInPlugin));
        h.AppendLine("namespace DisabledParents");
        h.AppendLine("{");
        h.AppendLine("\t// The game's own plugins, as Official.h names them, in their usual load order; an entry's file is its index here.");
        h.AppendLine("\tinline constexpr std::array<std::string_view, " + FILES.Length + "> FILES{ " + string.Join(", ", FILES.Select(f => "\"" + f + "\"").ToArray()) + " };");
        h.AppendLine();
        h.AppendLine("\t// The plugins the scan read, with their scripts: the table says nothing of any other's references.");
        h.AppendLine("\tinline constexpr std::array<std::string_view, " + scanned.Count + "> COVERED{ " + string.Join(", ", scanned.Select(f => "\"" + f + "\"").ToArray()) + " };");
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
        h.AppendLine("\t// Whether the scan read the plugin a_file (any case), its scripts with it (COVERED).");
        h.AppendLine("\tconstexpr bool Covers(std::string_view a_file)");
        h.AppendLine("\t{");
        h.AppendLine("\t\tconst auto file = FileIndex(a_file);");
        h.AppendLine("\t\treturn file < FILES.size() && std::ranges::find(COVERED, FILES[file]) != COVERED.end();");
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

    // a_safeOnly: the rows of src/game/QuestFilter.h's SAFE and SILENT quests alone (SafeScan.Load first).
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
                if (shared == "not shared" || (a_safeOnly && shared != "shared (safe)" && shared != "shared (silent)")) continue;
                bool first = true;
                var stages = MoveScan.Fragments(file, quest.Id);
                foreach (var stage in stages.Keys.OrderBy(k => k))
                {
                    var moves = MoveScan.StageMoves(file, quest, stage);
                    // a stage that makes the player a faction's enemy is no row here: HostileScan's (StageRules.h HOSTILE)
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

// The rows of src/game/StageRules.h's HOSTILE: every stage of every quest a friend's stage is set on (shared by its type,
// a finale, a dialogue quest, and with SafeScan loaded QuestFilter.h's SAFE and SILENT; radiants and PER_GAME quests are
// never set here) that makes the player a faction's enemy, itself or through what it calls and sets (SafeScan.StageHostile,
// walked as -Moves walks: its fragment, the script functions it calls, the stages it sets or starts of any quest), with
// the faction: the property its script names (SafeScan.HostileFaction), resolved to a form ID in the VMAD of the quest
// whose fragment or script makes the call (the last quest the chain names, else the stage's own; GearScan.Quest.Props),
// and failing that by its editor ID among the masters' factions (said in the comment: "by editor ID"). One row per
// stage, the first way found; a stage that turns two factions has only the first (DN049's x49 turn one each). A stage 0
// that isn't its quest's start-up stage is left out, as RuleScan leaves them. Unresolved ones are listed at the end, as
// are the counts by how the quest is shared. Sorted by file, quest and stage.
public static class HostileScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }

    // The property's forms in a quest's VMAD, by its name, its name without a leading p, or with one.
    static List<uint> Prop(GearScan.Quest q, string a_name)
    {
        List<uint> ids;
        if (q.Props.TryGetValue(a_name, out ids)) return ids;
        if (a_name.Length > 1 && (a_name[0] == 'p' || a_name[0] == 'P') && q.Props.TryGetValue(a_name.Substring(1), out ids)) return ids;
        if (q.Props.TryGetValue("p" + a_name, out ids)) return ids;
        return null;
    }

    // The factions (distinct) the calling fragment passes to the function the chain's first script hop names, resolved as
    // Run resolves a property (the caller's VMAD, else by editor ID); empty when the chain has no script hop or nothing is
    // passed that resolves.
    static List<uint> CallArguments(string[] a_hops, int a_file, GearScan.Quest a_quest, int a_stage, Dictionary<string, uint> a_factions)
    {
        var result = new List<uint>();
        GearScan.Quest caller = a_quest; int callerFile = a_file, callerStage = a_stage; string fn = null;
        for (int k = 0; k < a_hops.Length - 1; k++)
        {
            var parts = a_hops[k].Split(' ');
            int s;
            if (parts.Length == 1 && int.TryParse(parts[0], out s)) { callerStage = s; continue; }
            var q = parts.Length == 2 ? MoveScan.QuestByEdid(parts[0]) : null;
            if (q != null && int.TryParse(parts[1], out s)) { caller = q.Item1; callerFile = q.Item2; callerStage = s; continue; }
            if (a_hops[k].IndexOf('.') >= 0) { fn = a_hops[k].Substring(a_hops[k].LastIndexOf('.') + 1); break; }
        }
        if (fn == null) return result;
        List<GearScan.Func> funcs;
        if (!MoveScan.Fragments(callerFile, caller.Id).TryGetValue(callerStage, out funcs)) return result;
        foreach (var f in funcs)
            foreach (var i in f.Code)
            {
                int first;
                if (i.Op == 23 && Is(i.Args[0], fn)) first = 3; else if (i.Op == 24 && Is(i.Args[0], fn)) first = 2; else if (i.Op == 25 && Is(i.Args[1], fn)) first = 3; else continue;
                foreach (var a in i.Args.Skip(first))
                {
                    var n = a.Replace("::", ""); if (n.EndsWith("_var", StringComparison.OrdinalIgnoreCase)) n = n.Substring(0, n.Length - 4);
                    var ids = Prop(caller, n);
                    uint id;
                    if (ids != null && ids.Count > 0) id = ids[0];
                    else if (!a_factions.TryGetValue(n, out id) && !(n.Length > 1 && (n[0] == 'p' || n[0] == 'P') && a_factions.TryGetValue(n.Substring(1), out id))) continue;
                    if (!a_factions.ContainsValue(id)) continue;  // a property that is no faction (the quest, a message)
                    if (!result.Contains(id)) result.Add(id);
                }
            }
        return result;
    }

    // Stages whose faction the walk can't name: the script reads it from an array by a number the fragment passes
    // (mq00script.MQFactionKickoutPlayer(FactionNumber) sets MQ00's Factions[FactionNumber - 1] on the player: 1 the
    // Brotherhood, 3 the Railroad, 4 the Institute; MQ207's 5 sets its 200, which passes 4), or sets every entry of one
    // (DLC04GZMainQuest 10's Startup sets each of GalacticZoneEnemyFactions on the player), read by hand in those
    // scripts: the quest and stage, the quest whose VMAD holds the array, the array property, and the index (-1: every
    // entry, one row each). Each prints with "[by hand]".
    // Not here yet: DLC04MQ05 8 (Power Play) sets the snubbed gang on the player through DLC04GangSetup, which reads the gang
    // from the DLC04EnemyGang global: one of the three gangs' factions (the Disciples, the Operators, the Pack), none of them
    // a property the walk can name. Once the stage carries that global (the change that shares DLC04MQ05 with it), a
    // BY_HAND entry listing the three gang factions makes the hold wait while any gang member is near; until then the
    // stage has no row and lands at once far away.
    class ByHand { public string Quest; public int Stage; public string PropertyQuest; public string Property; public int Index; }
    static readonly ByHand[] BY_HAND = {
        new ByHand { Quest = "MQ00", Stage = 10, PropertyQuest = "MQ00", Property = "Factions", Index = 0 },
        new ByHand { Quest = "MQ00", Stage = 30, PropertyQuest = "MQ00", Property = "Factions", Index = 2 },
        new ByHand { Quest = "MQ00", Stage = 40, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "MQ207", Stage = 5, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "MQ207", Stage = 200, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "MQ302", Stage = 400, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "InstKickOut", Stage = 100, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "DLC04GZMainQuest", Stage = 10, PropertyQuest = "DLC04GZMainQuest", Property = "GalacticZoneEnemyFactions", Index = -1 },
    };

    // The factions a BY_HAND entry names for this quest's stage, or null.
    static List<uint> Hand(GearScan.Quest a_quest, int a_stage)
    {
        var entry = BY_HAND.FirstOrDefault(h => Is(h.Quest, a_quest.Edid) && h.Stage == a_stage);
        var owner = entry == null ? null : MoveScan.QuestByEdid(entry.PropertyQuest);
        List<uint> ids;
        if (owner == null || !owner.Item1.Props.TryGetValue(entry.Property, out ids)) return null;
        if (entry.Index < 0) return ids;
        return entry.Index < ids.Count ? new List<uint> { ids[entry.Index] } : null;
    }

    public static string Run(string data)
    {
        MoveScan.Load(data);
        var factions = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
        for (int f = 0; f < GearScan.MASTERS.Length; f++)
            foreach (var kv in GearScan.Factions(Path.Combine(data, GearScan.MASTERS[f].Esm), f)) if (!factions.ContainsKey(kv.Key)) factions[kv.Key] = kv.Value;
        var sb = new StringBuilder();
        var left = new List<string>();
        var unresolved = new List<string>();
        var counts = new SortedDictionary<string, int>();
        int rows = 0, byEdid = 0;
        var dismisses = new StringBuilder();  // StageRules.h DISMISSES: the HOSTILE stages that turn the faction's companion on the player too
        int dismissals = 0;
        var unresolvedDismissals = new List<string>();  // a dismissal whose chain names no companion quest: no row, read it by hand
        for (int file = 0; file < GearScan.MASTERS.Length; file++)
        {
            foreach (var quest in MoveScan.perFile[file].Values.OrderBy(q => q.Id & 0xFFFFFF))
            {
                var shared = QuestScan.Shared(quest, file);
                if (shared == "not shared" || !QuestScan.ReachesFriend(quest, file)) continue;
                bool first = true, firstDismissal = true;
                foreach (var stage in MoveScan.Fragments(file, quest.Id).Keys.OrderBy(k => k))
                {
                    var chain = SafeScan.StageHostile(file, quest, stage);
                    if (chain == null) continue;
                    var dismissal = SafeScan.StageDismisses(file, quest, stage);
                    if (dismissal != null && (stage != 0 || quest.Startup.Contains(0)))
                    {
                        var dhops = dismissal.Split(new[] { " -> " }, StringSplitOptions.None);
                        var deffect = dhops[dhops.Length - 1];
                        deffect = deffect.Substring(deffect.LastIndexOf(": ", StringComparison.Ordinal) + 2);
                        var who = SafeScan.DismissedCompanion(dismissal);
                        var alias = who == null ? null : who.Item1.Aliases.First(a => a.Id == who.Item3);
                        if (who == null)
                            unresolvedDismissals.Add(string.Format("#   {0} {1:X6} {2} {3}: {4}{5}", file, quest.Id & 0xFFFFFF, stage, quest.Edid, deffect, dhops.Length > 1 ? " (by " + dhops[0] + ")" : ""));
                        else
                            dismisses.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, 0x{4:X6}, {5} }},  // {6}{7}{8}: {9}'s {10}", file, quest.Id & 0xFFFFFF, stage, who.Item2, who.Item1.Id & 0xFFFFFF, who.Item3,
                                firstDismissal ? quest.Edid + ": " : "", deffect, dhops.Length > 1 ? " (by " + dhops[0] + ")" : "", who.Item1.Edid, alias.Name));
                        firstDismissal = false;
                        dismissals++;
                    }
                    var hops = chain.Split(new[] { " -> " }, StringSplitOptions.None);
                    var effect = hops[hops.Length - 1];
                    effect = effect.Substring(effect.LastIndexOf(": ", StringComparison.Ordinal) + 2);
                    var what = hops.Length > 1 ? effect + " (by " + hops[0] + ")" : effect;
                    if (stage == 0 && !quest.Startup.Contains(0))
                    {
                        left.Add(string.Format("#   {0} {1:X6} 0 {2}: {3}", file, quest.Id & 0xFFFFFF, quest.Edid, what));
                        continue;
                    }
                    var name = SafeScan.HostileFaction(chain);
                    // the quest whose script makes the call: the last hop naming a quest ("Edid N -> ..."), else this one
                    GearScan.Quest owner = quest;
                    for (int k = hops.Length - 2; k >= 0; k--)
                    {
                        var parts = hops[k].Split(' ');
                        var q = parts.Length == 2 ? MoveScan.QuestByEdid(parts[0]) : null;
                        if (q != null) { owner = q.Item1; break; }
                    }
                    var ids = name == null ? null : (Prop(owner, name) ?? Prop(quest, name));
                    string how = "";
                    uint faction;
                    var found = new List<uint>();
                    if (ids != null && ids.Count > 0) found.Add(ids[0]);
                    else if (name != null && factions.TryGetValue(name, out faction)) { found.Add(faction); how = " [by editor ID]"; byEdid++; }
                    else if (name != null && name.Length > 1 && (name[0] == 'p' || name[0] == 'P') && factions.TryGetValue(name.Substring(1), out faction)) { found.Add(faction); how = " [by editor ID]"; byEdid++; }
                    else if ((ids = Hand(quest, stage)) != null) { found.AddRange(ids); how = " [by hand]"; }
                    else
                    {
                        // The script's function takes the faction as its argument (mq00script.MQFactionKickoutPlayer(BoSFaction)): the
                        // one faction the fragment that calls it passes, read in the fragment of the stage the chain names before the
                        // call (its own stage, "N ->", another quest's, "Edid N ->", or this stage).
                        var passed = CallArguments(hops, file, quest, stage, factions);
                        if (passed.Count == 1) { found.Add(passed[0]); how = " [the call's argument]"; }
                        else { unresolved.Add(string.Format("#   {0} {1:X6} {2} {3}: {4}{5}", file, quest.Id & 0xFFFFFF, stage, quest.Edid, what, passed.Count > 1 ? " (several factions passed)" : "")); continue; }
                    }
                    // The call that turns the faction on the player (StageRules.h HostileCall): what QuestSync makes itself, without
                    // the stage, for a stage that also moves a player who didn't go with the friend (kMoves without kBridge).
                    var call = effect.StartsWith("SetPlayerEnemy", StringComparison.OrdinalIgnoreCase) ? "kSetPlayerEnemy"
                        : effect.StartsWith("AddToFaction", StringComparison.OrdinalIgnoreCase) ? "kAddToFaction" : "kSetEnemy";
                    foreach (var id in found)
                    {
                        sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, 0x{4:X6}, {8} }},  // {5}{6}{7}", file, quest.Id & 0xFFFFFF, stage, id >> 24, id & 0xFFFFFF,
                            first ? quest.Edid + ": " : "", what, how, call));
                        first = false;
                        rows++;
                    }
                    int n; counts.TryGetValue(shared, out n); counts[shared] = n + 1;
                }
            }
        }
        if (left.Count > 0)
        {
            sb.AppendLine("# left out, a stage 0 that is not its quest's start-up stage:");
            foreach (var l in left) sb.AppendLine(l);
        }
        if (unresolved.Count > 0)
        {
            sb.AppendLine("# not resolved to a faction (no row; add by hand):");
            foreach (var u in unresolved) sb.AppendLine(u);
        }
        sb.AppendLine("# " + rows + " rows (" + string.Join(", ", counts.Select(kv => kv.Value + " " + kv.Key).ToArray()) + "); " + byEdid + " by editor ID; " + unresolved.Count + " unresolved");
        sb.AppendLine("# DISMISSES: the HOSTILE stages that turn the faction's own companion on the player too (SafeScan.StageDismisses)");
        sb.Append(dismisses.ToString());
        sb.AppendLine("# " + dismissals + " rows");
        if (unresolvedDismissals.Count > 0)
        {
            sb.AppendLine("# dismissals whose chain names no companion quest (no row; read by hand):");
            foreach (var u in unresolvedDismissals) sb.AppendLine(u);
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
// the script functions it calls, the stages it sets or starts of the quests no friend's game is sent the stages of: not
// shared by their type, or radiants or PER_GAME, QuestScan.ReachesFriend; a shared quest's stage reaches a friend's game
// as the story's anyway), touches what each game keeps for itself (Direct): a
// companion's affinity, who is or may be a companion (or a stage of a companion's quest), a faction's hostility to
// another or to the player, a settlement's owner, or the player's whole inventory; or sends a story event, or starts a
// quest each game runs its own way (Target: a radiant, PER_GAME, repeatable, or with its own picks), which a friend's
// game would start there unshared. Less QuestFilter.h's DENIED (read
// from the file passed), a backstop: the quests the rule passes that are each player's all the same (survival's,
// the Combat Zone's fight, Nuka-World's park bookkeeping and outpost count: the six "denied:" lines). The rest of DENIED
// (MQ101, DogmeatQuest, DLC04RaiderKickout) the rule leaves out by itself, for a stage, and stays there in case it
// changes. The quests the rule passes but for their objectives (a location's quest, DN035) are SILENT's, printed after
// SAFE, less those denied by name or by hand or that stop early (WhySilent), each listed with why. Then,
// for each quest left out for a stage, the first stage found and its chain. Last, the payments reached through a shared
// stage, SAFE's, SILENT's or any other quest's a friend's stage is set on (Payments).
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
        { "factions", "changes factions' hostility" }, { "settlement", "changes a settlement's owner" },
        { "story", "sends a story event (the story manager starts quests each game picks itself)" },
        { "unshared", "starts a quest each game runs its own way (a radiant, PER_GAME, repeatable, or its own picks)" } };

    // What a function's own code does that makes its quest each player's, as "kind what" (a key of KINDS), or null: a
    // companion's affinity (an argument named *Affinity*, CA_*, or a call named *Affinity* that isn't a Get, Is or Trace);
    // companions (COMPANIONS: who is or can be one, a teammate, a dismissal); a faction's hostility to another or to the
    // player (FACTIONS, or adding to or taking out of a PlayerEnemy or PlayerAlly faction); a settlement's owner
    // (SetOwnedByPlayer); emptying the player's inventory (RemoveAllItems, UnequipAll on the player); or a story event
    // (SendStoryEvent: the quests the story manager starts from it, DN121_ChangeLocation's Minutemen request for one, are
    // started in each game with that game's own picks, and are not followed). a_outcomes false: a faction's hostility and a
    // settlement's owner are not looked for (the walk of an ALLOW_OUTCOMES quest, for what else its stages do).
    // a_companions true: a companion's affinity and who is a companion are not looked for either (the walk of an
    // ALLOW_KICKOUTS quest, whose kick-out turns a companion on the player as its outcome).
    static string Direct(List<GearScan.Instr> a_code) { return Direct(a_code, true); }

    static string Direct(List<GearScan.Instr> a_code, bool a_outcomes) { return Direct(a_code, a_outcomes, false); }

    static string Direct(List<GearScan.Instr> a_code, bool a_outcomes, bool a_companions)
    {
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Func<string, bool> player = v => players.Contains(v) || Bare(v).IndexOf("PlayerRef", StringComparison.OrdinalIgnoreCase) >= 0;
        foreach (var i in a_code)
        {
            foreach (var a in i.Args)
            {
                var b = Bare(a);
                if (!a_companions && (b.IndexOf("Affinity", StringComparison.OrdinalIgnoreCase) >= 0 || b.StartsWith("CA_", StringComparison.OrdinalIgnoreCase) || b.StartsWith("pCA_", StringComparison.OrdinalIgnoreCase))) return "affinity " + b;
            }
            if (i.Op != 23 && i.Op != 25 && i.Op != 13 && i.Op != 14) continue;
            if (i.Op == 13 || i.Op == 14) { if (player(i.Args[1])) players.Add(i.Args[0]); else players.Remove(i.Args[0]); continue; }
            var m = i.Op == 25 ? i.Args[1] : i.Args[0];
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(m, "GetPlayer")) { players.Add(i.Args[2]); continue; }
            if (COMPANIONS.Contains(m) && !a_companions) return "companion " + m;
            if (FACTIONS.Contains(m) && a_outcomes) return "factions " + m;
            if (SETTLEMENTS.Contains(m) && a_outcomes) return "settlement " + m + (i.Args.Count > 3 && Is(i.Args[3], "False") ? "(False)" : "");  // (False): the settlement is taken from the player
            if (Is(m, "SendStoryEvent") || Is(m, "SendStoryEventAndWait")) return "story " + m + " " + Bare(i.Op == 25 ? i.Args[0] : i.Args[1]);
            if (!a_companions && m.IndexOf("Affinity", StringComparison.OrdinalIgnoreCase) >= 0 && !m.StartsWith("Get", StringComparison.OrdinalIgnoreCase) && !m.StartsWith("Is", StringComparison.OrdinalIgnoreCase) && !m.StartsWith("Trace", StringComparison.OrdinalIgnoreCase)) return "affinity " + m;
            if (i.Op == 25) continue;
            if ((Is(m, "AddToFaction") || Is(m, "RemoveFromFaction") || Is(m, "SetFactionRank")) && i.Args.Skip(3).Any(a => Bare(a).IndexOf("PlayerEnemy", StringComparison.OrdinalIgnoreCase) >= 0 || Bare(a).IndexOf("PlayerAlly", StringComparison.OrdinalIgnoreCase) >= 0) && a_outcomes)
                return "factions " + m + " " + Bare(i.Args[3]);
            if ((Is(m, "GetReference") || Is(m, "GetRef") || Is(m, "GetActorRef") || Is(m, "GetActorReference")) && Bare(i.Args[1]).IndexOf("Player", StringComparison.OrdinalIgnoreCase) >= 0) { players.Add(i.Args[2]); continue; }
            if ((Is(m, "RemoveAllItems") || Is(m, "UnequipAll")) && player(i.Args[1])) return "empties " + m;
            players.Remove(i.Args[2]);
        }
        return null;
    }

    // Setting a stage of a companion's quest (its name as the rule's COMPANION), or starting one, is a companion's. So is
    // starting a quest each game runs its own way (EachGames: a radiant, PER_GAME, a repeatable one or one with an alias
    // each game fills, as Why leaves them out), by Start or by setting a stage of one the game doesn't start itself
    // (start game enabled, flag 0x1, runs in every game; the stage set, its fragment, is walked as any). A friend's game
    // would start it there unshared, with its own picks: DN017_ChangeLocation 0 starts the repeatable DN017 (as a story
    // event does, Direct's "story"). A quest the rule leaves out for what a stage does (MS07, DLC03FarHarborS01) is
    // fixed and runs once: the stages started or set are walked as any, and what its later stages do happens in the
    // friend's game as the friend plays it.
    static string Target(GearScan.Quest q, int a_file, string a_call)
    {
        if (COMPANION.IsMatch(System.Text.RegularExpressions.Regex.Replace(q.Edid, @"^DLC\d+_?", ""))) return "companion SetStage " + q.Edid;
        if (QuestScan.ReachesFriend(q, a_file) || !(Is(a_call, "Start") || (q.Flags & 1) == 0)) return null;
        var own = EachGames(q, a_file);
        return own == null ? null : "unshared " + a_call + " " + q.Edid + " (" + own + ")";
    }

    // Why a quest is each game's own by what it picks or how often it runs (Why's reasons for a radiant, PER_GAME, a
    // repeatable quest or an alias each game fills), or null.
    static string EachGames(GearScan.Quest q, int a_file)
    {
        uint local = q.Id & 0xFFFFFF;
        if (QuestScan.Radiants != null && QuestScan.Radiants.Contains(Tuple.Create(a_file, local))) return "a radiant";
        if (QuestScan.PerGame != null && QuestScan.PerGame.Contains(Tuple.Create(a_file, local))) return "PER_GAME";
        if ((q.Flags & 0x101) == 0) return "repeatable";
        var aliases = new Dictionary<int, GearScan.Alias>();
        foreach (var a in q.Aliases) if (!aliases.ContainsKey(a.Id)) aliases[a.Id] = a;
        if (q.Aliases.Any(a => PerGame(aliases, a.Id, 0))) return "an alias each game fills";
        return null;
    }

    // What each stage does, followed into the scripts it calls and the stages it sets or starts as -Moves follows them,
    // but for the stages of a quest every session with the story shares by its type (the story, a side or faction quest,
    // a dialogue quest, a finale: QuestScan.Shared) and is no radiant or PER_GAME quest, whose stages no friend's game is
    // sent (QuestScan.ReachesFriend): what one of those does a friend's game is sent anyway, SAFE or not.
    static readonly MoveScan.Walk walk = new MoveScan.Walk { Direct = Direct, Target = Target, Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Allows = AllowedStart, Unwalked = Radiant };

    // The starts of a quest each game runs its own way that are let through, by "<the quest whose stage starts it> <the quest
    // started>", with why: what the started quest does is each game's own and harmless twice (each game running it for its
    // own player is what a played-through game does). Its stages are walked as any other quest's (the rule holds for them),
    // but a radiant's (RadiantRules.h, shared by its run: Radiant): its stages reach a friend's game as that run's, and
    // a kick-out sets only a running one's failed stage (the walk doesn't see the IsRunning branch, and would read the
    // radiant's start-up as its own start of what it starts).
    static readonly Dictionary<string, string> ALLOW_STARTS = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "DN084 DN084_Overrides", "Mass Fusion's 10 starts DN084_Overrides (repeatable, no objectives; its one stage has no fragment): the quest companion's, the player's companion's and Dogmeat's packages for the descent and the control room, each game's own followers in each game, as a played-through game does" },
        { "BoSKickOut BoSMarshal", "KickOut sets BoSMarshal 255 if BoS304 runs (the Brotherhood's named people marshalled to the airport for the ending's battle, repeatable, no objectives): 255 reverts them to their real selves, a clean-up that runs only where the battle is on, as a played-through game does" },
        { "Inst307Fight BoSMarshal", "10 sets BoSMarshal 255 (the same clean-up, as the airport battle begins)" },
        { "BoSKickOut RECheckpoint*", "KickOut sets each running RECheckpoint* (the checkpoints' attackers after a faction's win, random encounters each game runs itself) to 600, which hides its objective and ends it, when this game's player won with the Brotherhood (RECheckpointMQWonBoS): a clean-up of each game's own encounters" },
        { "RRKickOut RECheckpoint*", "100 sets each running RECheckpoint* to 600 when this game's player won with the Railroad (RECheckpointMQWonRR): the same clean-up" },
        { "BoSKickOut BoSR*", "KickOut sets each running Brotherhood radiant (BoSR01-BoSR05: RadiantRules.h's, shared by their run) to its failed stage: a clean-up of a run this game has, as a played-through game does" },
        { "RRKickOut RRR05", "300 sets RRR05 9000 (a Railroad radiant, RadiantRules.h's, shared by its run): its failed stage, the same clean-up" },
        { "Inst307Fight Inst307FightCompanionOverrides", "10 sets Inst307FightCompanionOverrides 10 (repeatable, no objectives, named as a companion's but the airport battle's own: the player's companion's packages for the battle, each game's own follower in each game, as DN084_Overrides)" },
        { "BoSKickOut VFT", "KickOut sets VFT 355 (the vertibird signal grenades' travel, repeatable): the player's vertibird calls end with their expulsion (BoSFastTravelCanUse 0 too), each game's own rides, the kick-out's outcome in both" },
    };

    // A key ending in * matches by its prefix (a family of quests: the RECheckpoint* encounters).
    static bool AllowedStart(Tuple<GearScan.Quest, int> a_from, GearScan.Quest a_to, string a_call)
    {
        if (a_from == null) return false;
        var key = a_from.Item1.Edid + " " + a_to.Edid;
        return ALLOW_STARTS.ContainsKey(key) || ALLOW_STARTS.Keys.Any(k => k.EndsWith("*") && key.StartsWith(k.Substring(0, k.Length - 1), StringComparison.OrdinalIgnoreCase));
    }

    // A radiant shared by its run (RadiantRules.h): a start of one that ALLOW_STARTS lets through is not walked into.
    static bool Radiant(GearScan.Quest a_quest, int a_file)
    {
        return QuestScan.Radiants != null && QuestScan.Radiants.Contains(Tuple.Create(a_file, a_quest.Id & 0xFFFFFF));
    }
    // The same walk for an ALLOW_OUTCOMES quest: what its stages do but turn a faction hostile or friendly or give or take
    // a settlement (a companion, affinity, the inventory emptied, a story event or a quest each game runs its own way still
    // leave it out). It does not go into the workshop scripts a settlement given or taken (SetOwnedByPlayer) or a settler
    // added or removed (SETTLERS) runs: that is each game's workshop keeping its own settlement, as it does for any the
    // player owns there (the radio beacon's story event deep in WorkshopParentScript.UpdateRadioObject, which unassigning
    // a settler reaches only for a radio beacon assigned to them).
    static readonly HashSet<string> SETTLERS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "AddActorToWorkshop", "AddActorToWorkshopPUBLIC", "AddPermanentActorToWorkshopPUBLIC", "RemoveActorFromWorkshopPUBLIC" };
    static readonly MoveScan.Walk rest = new MoveScan.Walk { Direct = c => Direct(c, false), Target = Target, Follow = (q, file) => !QuestScan.ReachesFriend(q, file),
        Enter = name => !SETTLEMENTS.Contains(name) && !SETTLERS.Contains(name), Allows = AllowedStart, Unwalked = Radiant };
    // The same walk for an ALLOW_KICKOUTS quest: a companion's affinity and a dismissal are let through as well (Direct's
    // a_companions), and a stage of the companion's quest named with it (AllowedKickout: the Brotherhood's kick-out to
    // COMDanse, not any companion's); the rest of the rule holds.
    static readonly MoveScan.Walk kickout = new MoveScan.Walk {
        Direct = c => Direct(c, false, true), Target = Target,
        Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Enter = name => !SETTLEMENTS.Contains(name) && !SETTLERS.Contains(name),
        Allows = (from, to, call) => AllowedStart(from, to, call) || AllowedKickout(from, to), Unwalked = Radiant };

    static bool AllowedKickout(Tuple<GearScan.Quest, int> a_from, GearScan.Quest a_to)
    {
        return a_from != null && ALLOW_KICKOUTS.ContainsKey(a_from.Item1.Edid + " " + a_to.Edid);
    }

    // Whether a quest is an ALLOW_KICKOUTS kick-out (a key's first part).
    static bool Kickout(string a_edid)
    {
        return ALLOW_KICKOUTS.Keys.Any(k => k.StartsWith(a_edid + " ", StringComparison.OrdinalIgnoreCase));
    }

    // A stage that dismisses the player's companion, itself or through what it calls and sets (every stage it sets or starts,
    // shared or not, a companion's quest's too, as the hostile walk follows them): a call of DISMISSALS (the kick-outs reach
    // DismissCompanion through COMDanse 91, COMDeacon 91 and ComX688 91). HostileScan lists every HOSTILE stage that does as a
    // StageRules.h DISMISSES row, with the companion's quest the chain goes through (the last companion's quest it names) and
    // that quest's unique alias, the companion: a friend's such stage lands here as any, and the game's own kick-out takes
    // this player's companion with it as it does the chooser's (the maintainer's rule, 2026-11-11: as in the game); the row
    // only lets QuestSync warn the player once, before it lands, when the companion it dismisses is the one following them.
    // Who may be a companion (SetAvailableToBeCompanion, AllowCompanion), a teammate for a quest (SetPlayerTeammate) or an
    // affinity bump is no dismissal.
    static readonly HashSet<string> DISMISSALS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "DismissCompanion", "ClearCompanion", "DismissDogmeatCompanion", "ClearDogmeatCompanion" };

    static string Dismissal(List<GearScan.Instr> a_code)
    {
        foreach (var i in a_code)
        {
            if (i.Op != 23 && i.Op != 25) continue;
            var m = i.Op == 25 ? i.Args[1] : i.Args[0];
            if (DISMISSALS.Contains(m)) return "companion " + m;
        }
        return null;
    }

    // Not into what makes someone available as a companion or a teammate (COMPANIONS less the dismissals): Actor.AllowCompanion's
    // own code reaches a dismissal (BoS302 255's Danse), which is no kick-out of the player's.
    static readonly MoveScan.Walk dismisses = new MoveScan.Walk { Direct = Dismissal, Enter = name => !COMPANIONS.Contains(name) || DISMISSALS.Contains(name) };

    // How a stage turns a companion against the player (the first way found), or null.
    public static string StageDismisses(int a_file, GearScan.Quest a_quest, int a_stage) { return dismisses.Stage(a_file, a_quest, a_stage); }

    // The companion StageDismisses' chain dismisses: the last companion's quest the chain names as a hop ("COMDanse 91 ->"), and
    // the id of its unique alias (the companion: COMDanse's Danse, COMDeacon's Deacon, ComX688's X6-88), or null when the chain
    // names none (a dismissal in a script that is no companion quest's, or a quest with no unique alias).
    public static Tuple<GearScan.Quest, int, int> DismissedCompanion(string a_chain)
    {
        var hops = a_chain.Split(new[] { " -> " }, StringSplitOptions.None);
        for (int k = hops.Length - 1; k >= 0; k--)
        {
            var parts = hops[k].Split(' ');
            int s;
            if (parts.Length != 2 || !int.TryParse(parts[1], out s)) continue;
            var q = MoveScan.QuestByEdid(parts[0]);
            if (q == null || !System.Text.RegularExpressions.Regex.IsMatch(System.Text.RegularExpressions.Regex.Replace(q.Item1.Edid, @"^DLC\d+_?", ""), COMPANION.ToString(), System.Text.RegularExpressions.RegexOptions.IgnoreCase)) continue;  // ComX688 is a COM quest too
            var alias = q.Item1.Aliases.FirstOrDefault(a => a.Fill == "unique");
            return alias == null ? null : Tuple.Create(q.Item1, q.Item2, alias.Id);
        }
        return null;
    }
    // What an ALLOW_OUTCOMES quest's stage does that is let through, for the scan's print: a faction's hostility, a
    // settlement given or taken, or a settler added or removed (the first found).
    static readonly MoveScan.Walk outcome = new MoveScan.Walk {
        Direct = c => {
            var d = Direct(c, true);
            if (d != null && (d.StartsWith("factions ") || d.StartsWith("settlement "))) return d;
            var s = c.FirstOrDefault(i => i.Op == 23 && SETTLERS.Contains(i.Args[0]));
            return s == null ? null : "settlement " + s.Args[0];
        },
        Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Enter = name => !SETTLEMENTS.Contains(name) && !SETTLERS.Contains(name) };

    // The quests whose stages' faction or settlement outcomes are shared with the story, by the maintainer's choice
    // (2026-11-06): with sQuests = safe one player's outcome is both players', as the shared story's kick-outs already
    // are; with it off each keeps their own. By editor ID, with why, each read in its stage fragments (the stages each
    // flagged one sets too). Why lets a stage that does only that through (rest); the rest of the rule holds for them,
    // and RuleScan makes a stage that turns a faction on the player kMoves (StageRules.h). These are the ones
    // with objectives (SAFE). Read and left out: DLC04DialogueHubologists (its other stages are each player's own
    // sessions: rads cast on the player and the extraction chair's idle played on them, the paid donations, the rank's
    // bonus spells), and three the rule still leaves out for another stage, a quest each game runs its own way it
    // starts (DLC04MQ05's park power and clean-up, DLC04DialogueHarvey 1000's DLC04REObjectCT03,
    // DLC04SafariAdventureQuest's DLC04SafariAdventurePOST). With them, by the maintainer's choice of 2026-11-11 (a
    // faction's kick-out is the chooser's for both; a far friend's hostile stage waits until none of the faction is near,
    // never skipped: StageRules.h HOSTILE), the base game's objective-less kick-outs and faction outcomes (SILENT's):
    // BoSKickOut, BoSKickOutSoft and RRKickOut, DN084 (Mass Fusion, whose 2 sets the Brotherhood's kick-out, and its two
    // final battles), BoSDansePostGame, Inst307Fight, MS07cEddiePostQuestLines, DN028 and DN117 (a settlement given). A
    // kick-out that turns the faction's own companion on the player (Danse, Deacon) is ALLOW_KICKOUTS'. Far Harbor's
    // endings and kick-out and Nuka-World's come in another change.
    static readonly Dictionary<string, string> ALLOW_OUTCOMES = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "BoSKickOut", "the Brotherhood's kick-out (20, and 30 for Mass Fusion's: BoSKickOutScript.KickOut): the player out of the faction and its enemy, its quests failed, the airport's workshop taken, Danse too (ALLOW_KICKOUTS); set by Inst307Fight 1, InstMassFusion 30 and DN084 2" },
        { "BoSKickOutSoft", "the Brotherhood's soft expulsion (10: BoSKickOutSoftScript.SoftKickOut, for a player who sided against them without a fight): their quests failed or set past, BoSPostQuest 10; no faction turned, no companion dismissed (read in its script: up to 0.16.1 denied by hand as \"their companion's too\")" },
        { "RRKickOut", "the Railroad's kick-out (100): the player out of the faction and its enemy, its people made mortal, Deacon too (ALLOW_KICKOUTS), Curie's quest moved past the Railroad (COMCurieQuest 310 if it runs); 75 Glory out of her factions; 300 the Railroad's quests failed" },
        { "DN084", "Mass Fusion: 2 sets BoSKickOut 30 (the Brotherhood's kick-out, by the Institute's path); 10 starts DN084_Overrides (ALLOW_STARTS, the followers' descent packages); the rest is the building's own fight and its agitator" },
        { "DN084_BoS303Battle", "Mass Fusion's final battle on the Brotherhood's side: 200 sets DN084 450" },
        { "DN084_Inst303Battle", "Mass Fusion's final battle on the Institute's side: 200 sets DN084 450" },
        { "BoSDansePostGame", "Danse after Blind Betrayal (set by BoS302 255): 10 clears him from the kick-out's alias, 20 takes him out of the Brotherhood, puts him in their enemies' faction and makes them hostile to the player while he follows them (DansePostQuestBoSHostilityScript, each game's own companion)" },
        { "Inst307Fight", "the Institute's airport battle (Airship Down): 0 moves the player to the airport (kMoves), 1 puts them in the Institute's faction, out of the Brotherhood's and sets BoSKickOut 20 (the Brotherhood's kick-out) and Inst307's stages; the rest is the battle's waves" },
        { "MS07cEddiePostQuestLines", "Eddie Winter after Long Time Coming: 20 turns his faction on the player" },
        { "DN028", "the D.B. Technical High School boss's room: 10 makes its raiders the other raiders' allies, 510 turns them on the player, 500 their scene" },
        { "DN117", "Spectacle Island: 10/11 give the player the manor's workbenches and take them back, 20/21 the exterior workshop (a settlement given or taken), 30/31 the build area" },
        { "DN036", "Egret Tours Marina: 500 turns Phyllis on the player, 550/600/700 (each ending) give the player the marina's workshop" },
        { "DN049", "the robot-run shops: each shop's robots turned on the player (x49), made peaceful (x50), shut down (x55) or destroyed (x56) by the player's choices; 711's pre-war money is kPays" },
        { "DLC03FarHarborS01", "Uncle Ken's settlement: 500 adds its two settlers to the workshop (and gives its reward), 425 takes Ken out of it" },
        { "DLC03FarHarborS02", "Echo Lake Lumber: 210 turns Malcolm on the player, 425 gives the player the workshop with its settlers" },
        { "DLC03FarHarborFF01", "Dalton Farm: 220 gives the player the workshop with its settlers" },
        { "DLC03Acadia_Institute", "the Institute's move on Acadia: 200 sets DLC03AcadiaDefense 100, Acadia turned on the player (and the story's DLC03MQ05 8990)" },
        { "DLC03_ADV017", "100 makes Erikson and his dogs the player's allies" },
        { "DLC03CoA_DialogueNucleus", "the Nucleus's talks: 70 sets DLC03CoA_Kickout 700, the Children of Atom turned on the player, as the story's kick-outs" },
        { "DLC04_KiddieKingdomMain", "Kiddie Kingdom: 740 starts the fight with Oswald (his faction on the player); 600 and 700 run the quest script's one-time Init, which sets the hostile ghouls on the player as they are anyway" },
        { "DLC04DialogueSettlement", "Nuka-Town's traders: 700 makes the traders and visitors the hub raiders' enemies and the player's allies (the raiders' end); 110's bloodworm meat is kPays" },
    };

    // The ALLOW_OUTCOMES quests whose kick-out turns a companion on the player as part of its outcome, by "<the kick-out
    // quest> <the companion's quest>", with why: their walk lets a companion's affinity, a dismissal and a stage of that
    // companion's quest (no other's) through as well (kickout). The kick-out is shared (the maintainer's choice of
    // 2026-11-11), and a friend's game runs it as the chooser's did, the companion included: as in the game, a Danse or
    // Deacon who has followed this player is dismissed and turned by it, the friend's choice being both players'
    // (StageRules.h DISMISSES only warns the player first, once, when the companion is the one following them); one who is
    // the host's is theirs alone anyway (src/game/Companions).
    static readonly Dictionary<string, string> ALLOW_KICKOUTS = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "BoSKickOut COMDanse", "KickOut (20 and 30), only for a Danse who has been the player's companion (HasBeenCompanionFaction): his affinity to -5000, COMDanse 91 dismisses him for good; and he joins the player's enemies unless already exiled (BoSDansePostGame 10)" },
        { "DN084 COMDanse", "2 sets BoSKickOut 30, whose KickOut reaches Danse as above" },
        { "Inst307Fight COMDanse", "1 sets BoSKickOut 20, whose KickOut reaches Danse as above" },
        { "RRKickOut COMDeacon", "100: Deacon's affinity to -5000, COMDeacon 91 dismisses him for good, and he is made mortal with the Railroad's people" },
        { "RRKickOut COMCurieQuest", "100 sets COMCurieQuest 310 if it runs: the Caretaker's alias filled with a generic Railroad agent, so Curie's quest can go on without the Railroad (nothing of the player's companion)" },
    };

    // The first stage of a quest that makes it each player's, by quest (file, form ID): its stage and the chain found.
    static readonly Dictionary<Tuple<int, uint>, string> found = new Dictionary<Tuple<int, uint>, string>();

    // A stage that makes the player an enemy of a faction (SetPlayerEnemy, SetEnemy with the player's faction and neither
    // side a friend to the other, or someone added to a PlayerEnemy faction), itself or through what it calls and sets, as
    // -Moves walks: HostileScan gives every shared quest's such stage a StageRules.h HOSTILE row, so a friend's waits here
    // until none of that faction is near this player. StartCombat is not one: a passing fight, not a faction's standing
    // (the audit of 2026-11-10: 12 SAFE/SILENT stages, DN092 13/14/76, DN102 26/27, MS05BPostQuest 20/30, DLC03_ADV017 8,
    // DLC04Grandchester 50, DLC04_KiddieKingdomMain 570/590, go through as any other stage).
    // Faction.SetEnemy(akOther, abSelfIsNeutralToOther, abOtherIsNeutralToSelf): with either True that side is made neutral
    // instead (DN049's robots made peaceful again: SetEnemy(PlayerFaction, True, False), then StopCombat).
    static string Hostile(List<GearScan.Instr> a_code)
    {
        // a temp assigned or cast from a property (temp5 = DLC04GZFaction as Faction) is named by the property, for HostileScan
        var sources = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        Func<string, string> named = v => { string s; return sources.TryGetValue(v, out s) ? Bare(s) : Bare(v); };
        foreach (var i in a_code)
        {
            if ((i.Op == 13 || i.Op == 14) && i.Args.Count > 1) { if (!i.Args[1].StartsWith("::temp", StringComparison.OrdinalIgnoreCase)) sources[i.Args[0]] = i.Args[1]; else sources.Remove(i.Args[0]); continue; }
            if (i.Op != 23) continue;
            var m = i.Args[0];
            if (Is(m, "SetPlayerEnemy") && !(i.Args.Count > 3 && Is(i.Args[3], "False"))) return "SetPlayerEnemy " + named(i.Args[1]);
            if (Is(m, "SetEnemy") && !i.Args.Skip(4).Any(a => Is(a, "True"))
                && i.Args.Where((a, k) => k == 1 || k == 3).Any(a => Bare(a).IndexOf("PlayerFaction", StringComparison.OrdinalIgnoreCase) >= 0))
                return "SetEnemy " + named(i.Args[1]) + " " + (i.Args.Count > 3 ? named(i.Args[3]) : "");
            if (Is(m, "AddToFaction") && i.Args.Skip(3).Any(a => Bare(a).IndexOf("PlayerEnemy", StringComparison.OrdinalIgnoreCase) >= 0)) return "AddToFaction " + named(i.Args[3]);
            if (i.Args.Count > 2) sources.Remove(i.Args[2]);
        }
        return null;
    }

    static readonly MoveScan.Walk hostile = new MoveScan.Walk { Direct = Hostile };

    // How a stage makes the player a faction's enemy (the first way found), or null.
    public static string StageHostile(int a_file, GearScan.Quest a_quest, int a_stage) { return hostile.Stage(a_file, a_quest, a_stage); }

    // The faction StageHostile's answer turns on the player, as its script names it: SetPlayerEnemy's faction, SetEnemy's
    // side that isn't the player's, AddToFaction's PlayerEnemy faction (the actor added is its member then, so a friend's
    // stage waits while any member of that faction is near).
    public static string HostileFaction(string a_chain)
    {
        var effect = a_chain.Substring(a_chain.LastIndexOf(": ", StringComparison.Ordinal) + 2).Split(' ');
        if (effect.Length < 2) return null;
        if (!Is(effect[0], "SetEnemy")) return effect[1];
        for (int k = 1; k < effect.Length; k++) if (effect[k].Length > 0 && effect[k].IndexOf("PlayerFaction", StringComparison.OrdinalIgnoreCase) < 0) return effect[k];
        return null;
    }

    // Why a quest is not in SAFE ("" it is), as the rule reads it in order; "no objectives" for one that passes all else.
    static string Why(GearScan.Quest q, int a_file, HashSet<Tuple<int, uint>> a_radiants, HashSet<Tuple<int, uint>> a_perGame)
    {
        uint local = q.Id & 0xFFFFFF;
        if (QuestScan.Shared(q, a_file) != "not shared") return "shared by its type";
        if (a_radiants.Contains(Tuple.Create(a_file, local))) return "a radiant (RadiantRules.h)";
        if (a_perGame.Contains(Tuple.Create(a_file, local))) return "each game's (PER_GAME)";
        var bare = System.Text.RegularExpressions.Regex.Replace(q.Edid, @"^DLC\d+_?", "");
        if (COMPANION.IsMatch(bare)) return "a companion's";
        if ((q.Flags & 0x101) == 0) return "repeatable";
        var aliases = new Dictionary<int, GearScan.Alias>();
        foreach (var a in q.Aliases) if (!aliases.ContainsKey(a.Id)) aliases[a.Id] = a;
        if (q.Aliases.Any(a => PerGame(aliases, a.Id, 0))) return "an alias each game fills";
        var outcomes = ALLOW_OUTCOMES.ContainsKey(q.Edid);
        foreach (var stage in MoveScan.Fragments(a_file, q.Id).Keys.OrderBy(k => k))
        {
            var each = (outcomes && Kickout(q.Edid) ? kickout : outcomes ? rest : walk).Stage(a_file, q, stage);
            if (each == null) continue;
            found[Tuple.Create(a_file, local)] = stage + " " + each;
            var last = each.Substring(each.LastIndexOf(": ", StringComparison.Ordinal) + 2);
            return "a stage that " + KINDS[last.Substring(0, last.IndexOf(' '))];
        }
        if (q.Objectives == 0) return "no objectives";
        return "";
    }

    // The objective-less quests (those the rule passes but for their objectives) left out of SILENT by their name (the
    // editor ID less its DLCnn_ prefix), as families, with why: each pattern and its reason.
    static readonly Tuple<string, string>[] DENY_PATTERNS = {
        Tuple.Create(@"^(Achievements|Patch|Tutorial|Crafting$|PerksQuest$|modskill_)", "the game's own systems: achievements, patches, tutorials, crafting, perks"),
        Tuple.Create(@"(Holotapes|Radio|Legendary|CustomItemQuest|AspirationalItems)", "items, holotapes and radio stations each game hands out itself"),
        Tuple.Create(@"^(HC_|Player)|PowerArmorRecall|^SystemPowerArmor", "the player's own state: survival, the player's quests, their power armor"),
        Tuple.Create(@"^(Debug|CC_|VFT|zFAB$|2Djam)", "debug and test quests"),
        Tuple.Create(@"^(Conv|IntroScene|V81_Conv_|AO(_|$)|EE(_|$)|CabotHouse)|CreatureDialogue|Hellos$|DrinkingBuddy", "ambient chatter"),
        Tuple.Create(@"^BoSPrydwenScene_|^BoSScene_|^BoSIdleHandler|^InstituteSS\d|^SettlementSS\d|Scenes$|IntroScene$|Sermon$|HolySite$|^NickScene|^V81_Lesson_", "ambient scenes"),
        Tuple.Create(@"^(CA_|CIS_)", "a companion's"),
        Tuple.Create(@"(Manager|Tracker|Parent|ParentQuest|Template|TEMPLATE|SharedInfos?|PostQuestFlags|Disable)$|MasterQuest|^(EncounterManagementSystem|EMSystemQuest)$|^Common", "bookkeeping other quests use: managers, trackers, parents, templates, shared lines"),
        Tuple.Create(@"^RE[A-Z]", "random encounters, each game's own"),
        Tuple.Create(@"BotModQuest|Workshop", "workshops and robot building: each player's settlements and robots") };

    // The objective-less quests left out of SILENT by hand, by editor ID, with why: what a stage of theirs does (read
    // in their fragments and the scripts these call, as the walk does but deeper) is each player's, or they are
    // ambient.
    static readonly Dictionary<string, string> DENY_SILENT = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "BoSDialoguePrydwen", "the player's Brotherhood rank and what it unlocks" },
        { "BoSDialogueSquire", "ambient chatter with the player" },
        { "CaravanBase", "each game's caravans, and it pays caps" },
        { "DLC03_V118_PearlHiring", "Pearl follows the player; factions' hostility to the player" },
        { "DLC04GZLocationLoaded", "a faction's hostility to the player" },
        { "DLC04GZTrackingQuest", "the star cores the player holds; a faction's hostility to the player" },
        { "DLC04GZVaultTec", "a faction's hostility to the player" },
        { "DLC04MQ01Gauntlet", "each player's own run of the Gauntlet: takes and gives gear, moves the player" },
        { "DLC04MQ04TutorialQuest", "help messages" },
        { "DLC04NukaCade", "arcade tokens and an achievement, the player's own" },
        { "DLC04NukaPatch", "a patch's bookkeeping" },
        { "DLC04RaiderControllerQuest", "the raider gangs' bookkeeping: ranks, the player's standing" },
        { "DN011OverdueBooks", "each player's own book returns and their reward" },
        { "DN130-RR302", "a companion's affinity event" },
        { "DN136_Attack", "teleports the companion and Dogmeat into the Institute with the player" },
        { "DN151_Tracking", "the tracker of DN151, a Minutemen request each game picks itself (PER_GAME)" },
        { "DNPrime_BoS304", "the Institute attack's teleports, as DN136_Attack's" },
        { "FFGoodneighbor01MemorySequence", "the memory lounger: fades out, takes the controls and hides the player" },
        { "FFGoodneighbor08", "its 200, set by each game's own timer, ends the event (300) when this game's player is away from Goodneighbor: a far friend would end it in the host's game" },
        { "GenericGoHome", "a tool other quests' scripts use for their own actors" },
        { "InstituteFirstVisitQuest", "each player's own first visit" },
        { "InstituteTeleportEffects", "teleports the player" },
        { "MQ302Post", "after the ending: teleports, companions, affinity and factions" },
        { "MS04PostQuest", "Kent's costume upgrades take and give the player's Silver Shroud gear, and pay caps" },
        { "MS07cGreetingHandler", "a companion's (Nick's greeting)" },
        { "MS07NickHateHandler", "a companion's (Nick's affinity)" },
        { "MinutemenCentralQuest", "the player's own Minutemen: settlements, radio, recruits" },
        { "RRAct3Pickup", "teleports the player into the Institute and adds them to its faction" } };

    // Why an objective-less quest is not in SILENT ("" it is).
    static string WhySilent(GearScan.Quest q, int a_file)
    {
        if (q.Stages == 0) return "no objectives, no stages";
        var bare = System.Text.RegularExpressions.Regex.Replace(q.Edid, @"^DLC\d+_?", "");
        foreach (var d in DENY_PATTERNS)
            if (System.Text.RegularExpressions.Regex.IsMatch(bare, d.Item1)) return "denied by name: " + d.Item2;
        string why;
        if (DENY_SILENT.TryGetValue(q.Edid, out why)) return "denied by hand: " + why;
        why = StopsEarly(q, a_file);
        if (why != null) return STOPS_EARLY + why;
        return "";
    }

    const string STOPS_EARLY = "stops early: ";

    // A stage whose own fragment stops the quest (Stop on itself) below a stage whose fragment does more than stop it, as
    // "<stop> stops it, below <stage>", or null. A friend catching up is sent the host's stages done before the session
    // in ascending order (the game keeps no order of them), and once a silent quest ran and stopped in a game, the rest
    // of it is dropped there: MS07Intro's 350 would stop it before its 400 starts Nick's case files there. Only the
    // fragment's own code is read; a stop through a function it calls is not seen.
    static string StopsEarly(GearScan.Quest q, int a_file)
    {
        Func<GearScan.Instr, bool> stop = i => i.Op == 23 && Is(i.Args[0], "Stop") && (Is(i.Args[1], "self") || Is(i.Args[1], "kmyQuest") || Is(i.Args[1], "__temp"));
        Func<GearScan.Instr, bool> call = i => i.Op == 23 || i.Op == 24 || i.Op == 25;
        var stages = MoveScan.Fragments(a_file, q.Id);
        foreach (var s in stages.Where(kv => kv.Value.Any(f => f.Code.Any(stop))).Select(kv => kv.Key).OrderBy(k => k))
            foreach (var t in stages.Where(kv => kv.Key > s && kv.Value.Any(f => f.Code.Any(i => call(i) && !stop(i)))).Select(kv => kv.Key).OrderBy(k => k))
                return s + " stops it, below " + t;
        return null;
    }

    // Makes the SAFE set (QuestScan.Safe) and the SILENT one (QuestScan.Silent), less DENIED, and returns each quest's
    // verdict, by file and form ID: "" SAFE, "silent" SILENT.
    public static SortedDictionary<Tuple<int, uint>, string> Load(string data, string a_questFilter, string a_radiantRules)
    {
        MoveScan.Load(data);
        QuestScan.Safe = null;
        QuestScan.Silent = null;
        var radiants = new HashSet<Tuple<int, uint>>(System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(a_radiantRules), "Entry\\{\\s*(\\d),\\s*0x([0-9A-Fa-f]+),\\s*\"")
            .Cast<System.Text.RegularExpressions.Match>().Select(m => Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16) & 0xFFFFFF)));
        var perGame = Table(a_questFilter, "PER_GAME");
        QuestScan.Radiants = radiants;
        QuestScan.PerGame = perGame;
        var denied = Table(a_questFilter, "DENIED");
        var verdicts = new SortedDictionary<Tuple<int, uint>, string>();
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var q in MoveScan.perFile[file].Values)
            {
                var key = Tuple.Create(file, q.Id & 0xFFFFFF);
                var why = Why(q, file, radiants, perGame);
                if (why == "no objectives") { why = WhySilent(q, file); if (why == "") why = "silent"; }
                verdicts[key] = (why == "" || why == "silent") && denied.Contains(key) ? "denied (DENIED)" : why;
            }
        QuestScan.Safe = new HashSet<Tuple<int, uint>>(verdicts.Where(kv => kv.Value == "").Select(kv => kv.Key));
        QuestScan.Silent = new HashSet<Tuple<int, uint>>(verdicts.Where(kv => kv.Value == "silent").Select(kv => kv.Key));
        return verdicts;
    }

    // The (file, quest, stage) of the rows of src/game/StageRules.h's RULES that have a flag (kPays, kFree): from
    // "RULES[] = {" to its "};".
    public static HashSet<Tuple<int, uint, int>> Pays(string a_stageRules, string a_flag = "kPays")
    {
        var text = File.ReadAllText(a_stageRules);
        int at = text.IndexOf("RULES[] = {", StringComparison.Ordinal);
        if (at < 0) throw new Exception("RULES not found in " + a_stageRules);
        var body = text.Substring(at, text.IndexOf("};", at, StringComparison.Ordinal) - at);
        var rows = new HashSet<Tuple<int, uint, int>>();
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(body, @"\{\s*(\d+),\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*([^,}]+),\s*\d+\s*\}"))
            if (m.Groups[4].Value.Contains(a_flag)) rows.Add(Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16), int.Parse(m.Groups[3].Value)));
        return rows;
    }

    // Payments through a shared stage that were read by hand and pay nothing, by "shared quest stage -> how it is reached"
    // (as the scan prints it), with why.
    static readonly Dictionary<string, string> PAYS_CHECKED = new Dictionary<string, string> {
        { "MS13MarowskiRevenge 380 -> 400 -> MS13MarowskiRevenge 400: kPays",
            "400 takes the caps only when 380 is not done (its GetStageDone(380) branch jumps over the RemoveItem), and it is set from 380" } };

    // Payments through a shared stage. A stage that takes caps or ordinary items from the player is never set from a
    // friend's game (StageRules.h kPays), but a shared stage whose fragment sets it, or starts its quest, runs it here
    // when the friend's stage is set: DialogueDiamondCity 64 and 300 (Home Plate bought from Geneva or the mayor) set
    // FFDiamondCity02 10, which takes the house's price and gives the key and the house, and a friend's purchase was
    // made again in every game it was set in (up to 0.13.1). So every stage of every quest a friend's stage is set on
    // (shared by its type, a finale, a dialogue quest, SAFE's, SILENT's, or a radiant, shared by its run; not PER_GAME,
    // never shared) that is not itself a kPays row or the opening's (never set from a friend's) is walked as -Moves
    // walks them, into every stage it sets or starts, of any quest (shared or not; its own too), a few calls deep, and
    // each stage reached that has a kPays row, or whose own code or a function it calls takes caps or empties the
    // inventory (Payment), is listed, with how it is reached (the first way found for each stage). The walk does not
    // follow a branch on a stage being done, so a stage that pays only if another is not done is listed all the same:
    // those read by hand are PAYS_CHECKED's. The scan's last lines: the list, with the checked ones marked, and the
    // size of the rest as src/game/StageRules.h's SHARED_REACHES_PAYS, which tests/StageRulesTests.cpp checks is 0. A
    // stage listed is made a kPays row itself (a friend's is then never set here), a kFree row (a purchase set here
    // with its price at 0: StageRules.h PRICES; not walked, but one reached from another stage is a payment all the
    // same), or read and added to PAYS_CHECKED.
    public static string Payments(HashSet<Tuple<int, uint, int>> a_pays, HashSet<Tuple<int, uint, int>> a_free)
    {
        var walk = new MoveScan.Walk
        {
            Direct = Payment,
            Reaches = (file, q, stage) => a_pays.Contains(Tuple.Create(file, q.Id & 0xFFFFFF, stage)) ? q.Edid + " " + stage + ": kPays"
                : a_free.Contains(Tuple.Create(file, q.Id & 0xFFFFFF, stage)) ? q.Edid + " " + stage + ": kFree" : null
        };
        var lines = new List<string>();
        int open = 0, quests = 0, walked = 0;
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var key in MoveScan.perFile[file].Keys.OrderBy(k => k))
            {
                var q = MoveScan.perFile[file][key];
                if (QuestScan.Shared(q, file) == "not shared" || QuestScan.PerGame.Contains(Tuple.Create(file, key))) continue;
                quests++;
                foreach (var stage in MoveScan.Fragments(file, q.Id).Keys.OrderBy(k => k))
                {
                    if (a_pays.Contains(Tuple.Create(file, key, stage)) || a_free.Contains(Tuple.Create(file, key, stage)) || Opening(file, key, stage)) continue;
                    walked++;
                    var how = walk.Stage(file, q, stage);
                    if (how == null) continue;
                    string why;
                    bool checkedByHand = PAYS_CHECKED.TryGetValue(q.Edid + " " + stage + " -> " + how, out why);
                    if (!checkedByHand) open++;
                    lines.Add(string.Format("#   {0} 0x{1:X6} {2} {3} -> {4}{5}", file, key, q.Edid, stage, how, checkedByHand ? " (read by hand, pays nothing: " + why + ")" : ""));
                }
            }
        var sb = new StringBuilder();
        sb.AppendLine(string.Format("# stages reached from a shared stage that pay (StageRules.h kPays, {0} rows, and kFree, {1}; {2} stages of {3} shared quests walked): {4}, {5} read by hand",
            a_pays.Count, a_free.Count, walked, quests, lines.Count, lines.Count - open));
        foreach (var l in lines) sb.AppendLine(l);
        sb.AppendLine(string.Format("inline constexpr std::size_t SHARED_REACHES_PAYS = {0};", open));
        return sb.ToString();
    }

    // What a function's own code takes from the player that is a payment whatever the quest, or null: caps (RemoveItem or
    // SellItem of a Caps001 property) or the whole inventory (RemoveAllItems, UnequipAll). The stages that does in a quest
    // no friend's stage is set on were never read for kPays rows (only the shared quests' were), so the walk looks for it
    // in every function it reaches, a stage's own fragment too: what else a stage there takes (QuestScan.Takes) was read
    // by hand when this was written, from every shared stage, and was the quest's own item each time (MS19's cure, the
    // Institute's beacon grenades, MS11's parts, an alias's reference).
    static string Payment(List<GearScan.Instr> a_code)
    {
        var takes = QuestScan.Takes(new GearScan.Func { Code = a_code })
            .Where(t => t.Item2 == "" || ((Is(t.Item1, "RemoveItem") || Is(t.Item1, "SellItem")) && System.Text.RegularExpressions.Regex.IsMatch(t.Item2, "^p?Caps001$", System.Text.RegularExpressions.RegexOptions.IgnoreCase)))
            .Select(t => t.Item1 + (t.Item2 == "" ? "" : " " + t.Item2) + t.Item3).ToArray();
        return takes.Length == 0 ? null : "takes " + string.Join(", ", takes);
    }

    // The opening's stages (src/game/StageRules.h IsOpening: MQ102 below 10, all of DialogueVault111): never set from a
    // friend's game.
    static bool Opening(int a_file, uint a_quest, int a_stage)
    {
        return a_file == 0 && ((a_quest == 0x01CC2A && a_stage < 10) || a_quest == 0x01ED62);
    }

    // The ALLOW_OUTCOMES quests: each with its verdict (SAFE, SILENT or why it is still left out), why it is allowed, and
    // its stages that turn a faction hostile or friendly or give or take a settlement, with what each does (the effect at
    // the end of the walk's chain, and the first hop when it is reached through a call or another stage).
    static string Outcomes(SortedDictionary<Tuple<int, uint>, string> a_verdicts)
    {
        var sb = new StringBuilder();
        var lines = new List<string>();
        int safe = 0, silent = 0;
        foreach (var kv in a_verdicts)
        {
            var q = MoveScan.perFile[kv.Key.Item1][kv.Key.Item2];
            string why;
            if (!ALLOW_OUTCOMES.TryGetValue(q.Edid, out why)) continue;
            if (kv.Value == "") safe++;
            if (kv.Value == "silent") silent++;
            var what = new List<string>();
            foreach (var stage in MoveScan.Fragments(kv.Key.Item1, q.Id).Keys.OrderBy(k => k))
            {
                var each = outcome.Stage(kv.Key.Item1, q, stage);
                if (each == null) continue;
                var hops = each.Split(new[] { " -> " }, StringSplitOptions.None);
                var effect = hops[hops.Length - 1];
                effect = effect.Substring(effect.LastIndexOf(": ", StringComparison.Ordinal) + 2);
                what.Add(stage + " " + effect + (hops.Length > 1 ? " (by " + hops[0] + ")" : ""));
            }
            lines.Add(string.Format("#   {0} 0x{1:X6} {2} ({3}): {4}; {5}", kv.Key.Item1, kv.Key.Item2, q.Edid,
                kv.Value == "" ? "SAFE" : kv.Value == "silent" ? "SILENT" : "left out, " + kv.Value, why, string.Join(", ", what.ToArray())));
        }
        sb.AppendLine(string.Format("# outcomes shared with the story (ALLOW_OUTCOMES): {0} quests, {1} SAFE, {2} SILENT", lines.Count, safe, silent));
        foreach (var l in lines) sb.AppendLine(l);
        sb.AppendLine(string.Format("# kick-outs that turn a companion on the player too (ALLOW_KICKOUTS): {0}", ALLOW_KICKOUTS.Count));
        foreach (var kv in ALLOW_KICKOUTS) sb.AppendLine(string.Format("#   {0}: {1}", kv.Key, kv.Value));
        sb.AppendLine(string.Format("# starts let through (ALLOW_STARTS): {0}", ALLOW_STARTS.Count));
        foreach (var kv in ALLOW_STARTS) sb.AppendLine(string.Format("#   {0}: {1}", kv.Key, kv.Value));
        return sb.ToString();
    }

    // SAFE's rows, SILENT's, then what was left out and why.
    public static string Run(string data, string a_questFilter, string a_radiantRules, string a_stageRules)
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
        sb.AppendLine("# SILENT:");
        byFile = new int[GearScan.MASTERS.Length];
        stages = 0;
        foreach (var kv in verdicts.Where(kv => kv.Value == "silent"))
        {
            var q = MoveScan.perFile[kv.Key.Item1][kv.Key.Item2];
            byFile[kv.Key.Item1]++;
            stages += q.Stages;
            sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6} }},  // {2}", kv.Key.Item1, kv.Key.Item2, q.Edid));
        }
        sb.AppendLine(string.Format("# {0} quests ({1}), {2} stages", QuestScan.Silent.Count, string.Join(", ", GearScan.MASTERS.Select((m, i) => m.Esm + " " + byFile[i]).ToArray()), stages));
        sb.Append(Outcomes(verdicts));
        foreach (var kv in verdicts.Where(kv => kv.Value.StartsWith("denied by hand") || kv.Value.StartsWith(STOPS_EARLY)))
            sb.AppendLine(string.Format("# {0}: {1} 0x{2:X6} {3}", kv.Value, kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid));
        foreach (var kv in verdicts.Where(kv => kv.Value == "denied (DENIED)"))
            sb.AppendLine(string.Format("# denied: {0} 0x{1:X6} {2}", kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid));
        foreach (var g in verdicts.Values.Where(v => v != "" && v != "silent" && !v.StartsWith("denied (") && !v.StartsWith("denied by hand") && !v.StartsWith(STOPS_EARLY)).GroupBy(v => v).OrderBy(g => g.Key))
            sb.AppendLine(string.Format("# left out, {0}: {1}", g.Key, g.Count()));
        // what each quest left out for a stage does, and how its stage gets there, but the shared ones'
        foreach (var kv in found.Where(kv => verdicts[kv.Key].StartsWith("a stage")).OrderBy(kv => kv.Key))
            sb.AppendLine(string.Format("#   {0} 0x{1:X6} {2} {3}", kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid, kv.Value));
        // The radiants shared by their run (RadiantRules.h) whose stage takes a settlement from the player (SetOwnedByPlayer(False)),
        // directly or through what it calls: a friend's game runs that stage too, so the host's raid would cost the friend theirs.
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(a_radiantRules), "Entry\\{\\s*(\\d),\\s*0x([0-9A-Fa-f]+),\\s*\"[^\"]*\",\\s*Share::kShared"))
        {
            int file = int.Parse(m.Groups[1].Value);
            uint local = Convert.ToUInt32(m.Groups[2].Value, 16) & 0xFFFFFF;
            GearScan.Quest q;
            if (file >= MoveScan.perFile.Count || !MoveScan.perFile[file].TryGetValue(local, out q)) continue;
            foreach (var stage in MoveScan.Fragments(file, q.Id).Keys.OrderBy(k => k))
            {
                var each = walk.Stage(file, q, stage);
                if (each != null && each.EndsWith("SetOwnedByPlayer(False)") && each.IndexOf(" -> ", StringComparison.Ordinal) < 0)  // its own fragment: what the settlement code deeper in does only when a happiness drops
                    sb.AppendLine(string.Format("# shared radiant {0} 0x{1:X6} {2}: stage {3} takes a settlement from the player: {4}", file, local, q.Edid, stage, each));
            }
        }
        sb.Append(Payments(Pays(a_stageRules), Pays(a_stageRules, "kFree")));
        return sb.ToString();
    }
}
