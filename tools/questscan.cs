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
        public List<int> StageIds = new List<int>();  // each stage's index (INDX), in the record's order
        public List<int> ObjectiveIds = new List<int>();  // each objective's index (QOBJ), in the record's order
        public string Event = "";  // the story manager event that starts it (ENAM), if any
        public List<string> Scripts = new List<string>();  // the scripts attached to the quest itself (its VMAD), not its stage fragments' or aliases'
        public List<string> Fills = new List<string>();  // each alias's fill, name=forced, unique, external, created, event, fromalias, matching or empty
        public List<Alias> Aliases = new List<Alias>();  // the same, with each alias's ID and the alias it is filled from
        public List<int> Complete = new List<int>();  // the stages whose log entry completes the quest (QSDT flag 0x1)
        // The object properties of every script in the quest's VMAD (its own scripts, its stage fragments' and its aliases'),
        // by name: their forms as MASTERS index << 24 | form ID (a form of a file not in MASTERS is left out; a property of
        // several scripts keeps the first with values). What a fragment's SetEnemy(DLC03ChildrenofAtomFaction) names.
        public Dictionary<string, List<uint>> Props = new Dictionary<string, List<uint>>(StringComparer.OrdinalIgnoreCase);
        // The stage each stage fragment function runs for, by the VMAD's fragment table (ParentScan.QuestFragmentStages);
        // empty when the quest has no VMAD (no fragment runs), null when it can't be read (StageOf then goes by the
        // function's name).
        public Dictionary<string, int> FragmentStages = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
    }

    // The stage a function of a quest's fragment script runs for: the one its VMAD binds it to, or none (false) for a
    // function no stage binds, as the game never runs it. Without a fragment table read, by its name (Fragment_Stage_NNNN_...).
    public static bool StageOf(Quest a_quest, Func a_func, out int a_stage)
    {
        a_stage = -1;
        if (a_quest.FragmentStages != null) return a_quest.FragmentStages.TryGetValue(a_func.Name, out a_stage);
        return a_func.Name.StartsWith("Fragment_Stage_", StringComparison.OrdinalIgnoreCase) && a_func.Name.Length >= "Fragment_Stage_".Length + 4
            && int.TryParse(a_func.Name.Substring("Fragment_Stage_".Length, 4), out a_stage);
    }

    // An alias: its ID (references and locations share one numbering), name, fill (Fill), the alias it is made at or
    // found from (ALFA, ALNA, ALCA; -1 none) and the reference it is forced to (ALFR; 0 none), as the file writes it.
    public class Alias { public int Id; public string Name = ""; public string Fill = ""; public int Source = -1; public uint Forced; }

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
                        HashSet<string> alias = null; string aliasName = ""; int aliasId = -1, aliasSource = -1; uint aliasForced = 0; int lastStage = -1;
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
                                if (st == "INDX") { q.Stages++; lastStage = BitConverter.ToUInt16(body, d); if (ss >= 2) q.StageIds.Add(BitConverter.ToUInt16(body, d)); if (ss >= 3 && (body[d + 2] & 0x02) != 0) q.Startup.Add(lastStage); }
                                if (st == "QSDT" && ss >= 1 && (body[d] & 0x01) != 0 && lastStage >= 0 && !q.Complete.Contains(lastStage)) q.Complete.Add(lastStage);
                                if (st == "QOBJ") { q.Objectives++; if (ss >= 2) q.ObjectiveIds.Add(BitConverter.ToUInt16(body, d)); }
                                if (st == "VMAD") { q.Scripts = ParentScan.OwnScripts(body, d, ss); q.Props = ParentScan.QuestProperties(body, d, ss, names, fileIndex); q.FragmentStages = ParentScan.QuestFragmentStages(body, d, ss); }
                            }
                            // the aliases come last: each from its ALST, ALLS or ALCS to its ALED
                            if (st == "ALST" || st == "ALLS" || st == "ALCS") { alias = new HashSet<string>(); aliasName = ""; aliasId = ss >= 4 ? BitConverter.ToInt32(body, d) : -1; aliasSource = -1; aliasForced = 0; }
                            else if (alias != null)
                            {
                                if (st == "ALID") aliasName = Encoding.ASCII.GetString(body, d, Math.Max(0, ss - 1));
                                if ((st == "ALFA" || st == "ALNA") && ss >= 4) aliasSource = BitConverter.ToInt32(body, d);
                                if (st == "ALCA" && ss >= 2) aliasSource = BitConverter.ToInt16(body, d);
                                if (st == "ALFR" && ss >= 4) aliasForced = BitConverter.ToUInt32(body, d);
                                alias.Add(st);
                                if (st == "ALED")
                                {
                                    var fill = Fill(alias);
                                    q.Fills.Add(aliasName + "=" + fill);
                                    q.Aliases.Add(new Alias { Id = aliasId, Name = aliasName, Fill = fill, Source = aliasSource, Forced = aliasForced });
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
                    int stage;
                    if (!GearScan.StageOf(quest, f, out stage)) continue;  // a function no stage binds never runs
                    var hits = Takes(f).Select(t => t.Item1 + "(" + t.Item2 + t.Item3 + ")").ToList();
                    if (hits.Count == 0) continue;
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
                    int stage; if (!GearScan.StageOf(quest, f, out stage)) continue;
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
    // Funcs: the first function of each name (what a call finds); All: every function, in every state.
    class Script { public GearScan.Obj Obj; public Dictionary<string, GearScan.Func> Funcs = new Dictionary<string, GearScan.Func>(StringComparer.OrdinalIgnoreCase); public List<GearScan.Func> All = new List<GearScan.Func>(); }

    static readonly Dictionary<string, Script> scripts = new Dictionary<string, Script>(StringComparer.OrdinalIgnoreCase);

    // Every function of a script and of the scripts it extends, in every state; null when the script (or one it extends)
    // is not in the masters' archives.
    public static List<GearScan.Func> Functions(string a_type)
    {
        var list = new List<GearScan.Func>();
        for (int depth = 0; !string.IsNullOrEmpty(a_type) && depth < 16; depth++)
        {
            Script s;
            if (!scripts.TryGetValue(a_type, out s)) return Is(a_type, "Quest") || Is(a_type, "ReferenceAlias") || Is(a_type, "RefCollectionAlias") || Is(a_type, "ObjectReference") || Is(a_type, "Actor") || Is(a_type, "ScriptObject") ? list : null;
            list.AddRange(s.All);
            a_type = s.Obj.Parent;
        }
        return list;
    }

    // Every function of every script read (each script's own, in every state).
    public static IEnumerable<GearScan.Func> AllFunctions() { return scripts.Values.SelectMany(s => s.All); }

    // How a function's own code moves the player (MoveTo on the player, a fade out, the controls taken, a door they are
    // made to use), not through what it calls; null: it doesn't.
    public static string MovesItself(GearScan.Func f) { return Direct(f.Code); }

    // The names of the functions some script calls (callmethod, callparent, callstatic, and the names handed to
    // CallFunction, CallFunctionNoWait, CallGlobalFunction and CallGlobalFunctionNoWait as literals).
    static HashSet<string> called;
    // the dynamic calls whose function name is not a literal (any function may be the one called): where, by function
    static List<string> dynamicNames;

    public static bool Dynamic(string a_name) { return Is(a_name, "CallFunction") || Is(a_name, "CallFunctionNoWait") || Is(a_name, "CallGlobalFunction") || Is(a_name, "CallGlobalFunctionNoWait"); }

    // Whether a function can run in a game played through: the engine calls an event (On..., a remote event) and a fragment,
    // and a script calls any other by its name. One no script calls (Quest.SetAllStages) runs only from a developer's console.
    // Every function runs when some dynamic call's name is not a literal.
    public static bool Runs(GearScan.Func f)
    {
        if (called == null)
        {
            called = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            dynamicNames = new List<string>();
            foreach (var g in AllFunctions())
                foreach (var i in g.Code)
                {
                    string name = null; int at = -1;
                    if ((i.Op == 23 || i.Op == 24) && i.Args.Count > 0) { name = i.Args[0]; at = i.Op == 23 ? 3 : 2; }
                    if (i.Op == 25 && i.Args.Count > 1) { name = i.Args[1]; at = 4; }
                    if (name == null) continue;
                    called.Add(name);
                    if (!Dynamic(name)) continue;
                    if (at >= i.Args.Count || !i.Args[at].StartsWith("\"")) { dynamicNames.Add(g.Obj + "." + g.Name); continue; }
                    called.Add(i.Args[at].Trim('"'));
                }
        }
        if (dynamicNames.Count > 0) return true;
        return f.Name.StartsWith("On", StringComparison.OrdinalIgnoreCase) || f.Name.StartsWith("::remote_", StringComparison.OrdinalIgnoreCase) ||
               f.Name.StartsWith("Fragment_", StringComparison.OrdinalIgnoreCase) || called.Contains(f.Name);
    }

    // The dynamic calls whose function name is not a literal (Runs is then true of every function).
    public static IList<string> DynamicNames { get { Runs(new GearScan.Func { Name = "" }); return dynamicNames; } }

    // The script functions a call in f may run (callmethod on self or a variable whose type is a script, callparent,
    // callstatic): the one the walk finds, in every state, and those that override it in the scripts that extend the
    // receiver's type. Empty: a native function. False: the receiver's type is not known, or its script is not read.
    public static bool Callees(GearScan.Func f, GearScan.Instr i, out List<GearScan.Func> a_targets)
    {
        a_targets = new List<GearScan.Func>();
        string type, name;
        if (i.Op == 25 && i.Args.Count > 1) { type = i.Args[0]; name = i.Args[1]; }
        else if (i.Op == 24 && i.Args.Count > 0) { Script sc; type = scripts.TryGetValue(f.Obj, out sc) ? sc.Obj.Parent : null; name = i.Args[0]; }
        else if (i.Op == 23 && i.Args.Count > 1) { type = TypeOf(f, i.Args[1]); name = i.Args[0]; }
        else return false;
        if (string.IsNullOrEmpty(type)) return false;
        var chain = Functions(type);
        if (chain == null) return false;
        a_targets.AddRange(chain.Where(g => Is(g.Name, name)));
        if (i.Op == 23)
            foreach (var s in scripts.Values)
            {
                if (Is(s.Obj.Name, type)) continue;
                bool extends = false;
                Script p;
                for (var o = s.Obj.Parent; !extends && !string.IsNullOrEmpty(o) && scripts.TryGetValue(o, out p); o = p.Obj.Parent) extends = Is(o, type);
                if (extends) a_targets.AddRange(s.All.Where(g => Is(g.Name, name)));
            }
        return true;
    }

    // The arguments an instruction writes (its destinations), by index.
    public static IEnumerable<int> Dests(GearScan.Instr i)
    {
        if (i.Args.Count == 0) yield break;
        switch (i.Op)
        {
            case 23: case 25: case 28: if (i.Args.Count > 2) yield return 2; break;
            case 24: if (i.Args.Count > 1) yield return 1; break;
            case 34: case 35: case 40: case 41: if (i.Args.Count > 1) yield return 1; break;
            case 0: case 20: case 21: case 22: case 26: case 29: case 33: case 39: case 42: case 43: case 44: case 45: case 46: break;
            default: yield return 0; break;
        }
    }

    static HashSet<string> propsets;  // the property names some script sets
    static readonly Dictionary<string, bool> varWritten = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
    // Whether a script's variable (::Name_var) may hold other than what it starts with: a function of the script or of one
    // it extends writes it, or some script sets the property (propset, by its name, on any object).
    public static bool VarWritten(string a_type, string a_var)
    {
        bool b;
        if (varWritten.TryGetValue(a_type + "|" + a_var, out b)) return b;
        if (propsets == null)
        {
            propsets = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var g in AllFunctions()) foreach (var i in g.Code) if (i.Op == 29 && i.Args.Count > 0) propsets.Add(i.Args[0]);
        }
        var chain = Functions(a_type);
        b = chain == null || chain.Any(g => g.Code.Any(i => Dests(i).Any(d => Is(i.Args[d], a_var)))) || propsets.Contains(Bare(a_var));
        varWritten[a_type + "|" + a_var] = b;
        return b;
    }

    // What a script's own variable (one of its object's, by name, in it or a script it extends) starts with, or null: no
    // such variable.
    public static string Default(string a_type, string a_var)
    {
        for (int depth = 0; !string.IsNullOrEmpty(a_type) && depth < 16; depth++)
        {
            Script s;
            if (!scripts.TryGetValue(a_type, out s)) return null;
            string d;
            if (s.Obj.Types.ContainsKey(a_var)) return s.Obj.Defaults.TryGetValue(a_var, out d) ? d : "None";
            a_type = s.Obj.Parent;
        }
        return null;
    }

    public static readonly Dictionary<string, Tuple<GearScan.Quest, int>> byEdid = new Dictionary<string, Tuple<GearScan.Quest, int>>(StringComparer.OrdinalIgnoreCase);
    // per quest (file, id): its stage fragments, by stage
    static readonly Dictionary<Tuple<int, uint>, Dictionary<int, List<GearScan.Func>>> fragments = new Dictionary<Tuple<int, uint>, Dictionary<int, List<GearScan.Func>>>();

    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }
    static bool Own(string a_var) { return Is(a_var, "self") || Is(a_var, "kmyQuest") || Is(a_var, "__temp"); }

    // The function a call of a_func on a script of type a_type runs (its own or a parent's, a_owner the script that has it), or null.
    public static GearScan.Func Find(string a_type, string a_func, out string a_owner)
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

    // The script a script extends ("" none, null a script not read).
    public static string ParentOf(string a_script)
    {
        Script s;
        return scripts.TryGetValue(a_script, out s) ? s.Obj.Parent : null;
    }

    // The type of a function's parameter, local, or its script's variable or property (null: not known).
    public static string TypeOf(GearScan.Func f, string a_var)
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

    // The quest a property of a quest's scripts holds (its VMAD: the form itself, not a guess from the name), or null:
    // BoSKickOutScript's BoSDialogue is BoSDialoguePrydwen.
    static Tuple<GearScan.Quest, int> QuestOfProp(Tuple<GearScan.Quest, int> a_owner, string a_var)
    {
        if (a_owner == null) return null;
        var n = Bare(a_var);
        List<uint> ids;
        if (!a_owner.Item1.Props.TryGetValue(n, out ids) || ids.Count != 1) return null;
        int file = (int)(ids[0] >> 24);
        GearScan.Quest q;
        return file < perFile.Count && perFile[file].TryGetValue(ids[0] & 0xFFFFFF, out q) ? Tuple.Create(q, file) : null;
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
        // Whether a stage a function sets or starts (file, quest, stage) is left out of the walk altogether: neither judged nor
        // walked into (SafeScan: a stage a friend's game never runs, kPays or kEach, or one left unjudged by hand).
        public Func<int, GearScan.Quest, int, bool> Skip = (file, q, stage) => false;
        // Whether a script function called is walked into (by its name): SafeScan's walk of an ALLOW_OUTCOMES quest does not
        // go into what a settlement's own scripts do once it is given or taken (SetOwnedByPlayer) or gains or loses a settler.
        public Func<string, bool> Enter = name => true;
        // Told of each function the walk reaches, with the code it can reach there, whether or not Direct finds anything in
        // it: for a scan that wants every answer along the walk, not the first (GlobalScan's every read). A walk whose
        // Direct finds nothing caches nothing, so a function reached by several paths is told of each time.
        public Action<GearScan.Func, List<GearScan.Instr>> Visit;
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
            if (Visit != null) Visit(f, code);
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
                        if (Skip(target.Item2, target.Item1, st)) continue;
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

    // A script function's code, one instruction per line ("op args..."), for reading what a game function does by hand
    // (-Function Script.Function: FollowersScript.AllowCompanion). Found through the script's parents as a call is.
    public static string Dump(string data, string a_name)
    {
        Load(data);
        int dot = a_name.LastIndexOf('.');
        if (dot < 0) return "# usage: -Function <Script>.<Function>";
        string owner;
        var f = Find(a_name.Substring(0, dot), a_name.Substring(dot + 1), out owner);
        if (f == null) return "# " + a_name + ": no such function";
        var sb = new StringBuilder();
        sb.AppendLine(string.Format("# {0}.{1}({2}) in {3}", owner, f.Name, string.Join(", ", f.Params.ToArray()), f.State));
        foreach (var i in f.Code) sb.AppendLine(string.Format("{0,3} {1}", i.Op, string.Join(" ", i.Args.ToArray())));
        return sb.ToString();
    }

    // A quest's stage fragments, by stage (none: an empty one).
    public static Dictionary<int, List<GearScan.Func>> Fragments(int a_file, uint a_quest)
    {
        Dictionary<int, List<GearScan.Func>> stages;
        return fragments.TryGetValue(Tuple.Create(a_file, a_quest & 0xFFFFFF), out stages) ? stages : new Dictionary<int, List<GearScan.Func>>();
    }

    // The instructions a quest's stage fragments can run themselves (none: an empty list).
    public static List<GearScan.Instr> FragmentCode(int a_file, uint a_quest, int a_stage)
    {
        var code = new List<GearScan.Instr>();
        List<GearScan.Func> list;
        if (Fragments(a_file, a_quest).TryGetValue(a_stage, out list)) foreach (var f in list) code.AddRange(Reachable(f, null));
        return code;
    }

    // The stages a quest's stage sets or starts in its fragments' own code, by stage sets alone (SetStage on self or the
    // quest's script, or on another quest by its property's name, with that quest's start-up stages as Start gives), as
    // (file, quest, stage). What a script function the fragment calls sets is not followed.
    public static List<Tuple<int, GearScan.Quest, int>> StageSets(int a_file, GearScan.Quest a_quest, int a_stage)
    {
        var sets = new List<Tuple<int, GearScan.Quest, int>>();
        foreach (var i in FragmentCode(a_file, a_quest.Id, a_stage))
        {
            if (i.Op != 23 || i.Args.Count < 2 || !(Is(i.Args[0], "SetStage") || Is(i.Args[0], "Start"))) continue;
            var target = Own(i.Args[1]) ? Tuple.Create(a_quest, a_file) : QuestOf(i.Args[1]);
            if (target == null) continue;
            var own = target.Item1 == a_quest;
            int s;
            if (Is(i.Args[0], "SetStage") && i.Args.Count > 3 && int.TryParse(i.Args[3], out s)) sets.Add(Tuple.Create(target.Item2, target.Item1, s));
            if (!own) foreach (var st in target.Item1.Startup) sets.Add(Tuple.Create(target.Item2, target.Item1, st));  // starts it, if it isn't running
        }
        return sets;
    }

    // A stage that a stage's script sets: its quest (and file), the stage, and the stage whose fragment (or a function
    // it calls) sets it ("InstMassFusion 30").
    public class Reached { public int File; public GearScan.Quest Quest; public int Stage; public string By; }

    // Every stage that setting a stage sets, as deep as it goes (all of them, not the first found as Walk: each function
    // and each stage is walked once, breadth first): by its fragment and the script functions it calls (as Walk reads
    // them: the instructions a call can reach with its literal arguments, the quest's own script through kmyQuest or
    // self, any script a property or local is typed with), a SetStage on its own quest or on any other (the quest the
    // property holds in the VMAD of the quest whose script it is, else matched by the property's name, or by the
    // variable's type when that script is on one quest only), and the stages those set in
    // turn. A SetStage or Start of another quest that the game doesn't start by itself (not Start Game Enabled) starts
    // it if it isn't running: its start-up stages are set too, and so are a_quest's own when a_starts (a console's
    // setstage starts the quest). Every branch is taken that a literal argument doesn't decide: what a stage sets only
    // on a condition of the game it runs in is in the set. a_unresolved gets each SetStage or Start it could not follow
    // (on a variable that names no quest, or with a stage that is no literal). What it does not see is what Walk does
    // not: a scene, a timer or event a script waits for, a call through a variable whose type has no script.
    public static List<Reached> Sets(int a_file, GearScan.Quest a_quest, int a_stage, bool a_starts, List<string> a_unresolved)
    {
        var found = new HashSet<string>();
        var order = new List<Reached>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var todo = new Queue<Tuple<GearScan.Func, string[], Tuple<GearScan.Quest, int>, string>>();
        Action<int, GearScan.Quest, int, string> add = (file, quest, stage, by) =>
        {
            if (!found.Add(file + ":" + (quest.Id & 0xFFFFFF) + ":" + stage)) return;
            if (by != null) order.Add(new Reached { File = file, Quest = quest, Stage = stage, By = by });
            Dictionary<int, List<GearScan.Func>> all;
            List<GearScan.Func> list;
            if (fragments.TryGetValue(Tuple.Create(file, quest.Id & 0xFFFFFF), out all) && all.TryGetValue(stage, out list))
                foreach (var f in list) todo.Enqueue(Tuple.Create(f, (string[])null, Tuple.Create(quest, file), quest.Edid + " " + stage));
        };
        add(a_file, a_quest, a_stage, null);
        if (a_starts && (a_quest.Flags & 1) == 0) foreach (var s in a_quest.Startup) add(a_file, a_quest, s, a_quest.Edid + " " + a_stage + ", starting the quest");
        while (todo.Count > 0)
        {
            var item = todo.Dequeue();
            var f = item.Item1; var quest = item.Item3; var by = item.Item4;
            var key = f.Obj + "." + f.State + "." + f.Name + "(" + (item.Item2 == null ? "" : string.Join(",", item.Item2.Select(a => a ?? "?").ToArray())) + ")@" + (quest == null ? "" : quest.Item2 + ":" + quest.Item1.Id);
            if (!seen.Add(key)) continue;
            foreach (var i in Reachable(f, item.Item2))
            {
                if (i.Op != 23 && i.Op != 24 && i.Op != 25) continue;
                if (i.Op == 23 && (Is(i.Args[0], "SetStage") || Is(i.Args[0], "Start")))
                {
                    var own = Own(i.Args[1]);
                    var target = own ? quest : QuestOfProp(quest, i.Args[1]) ?? QuestOf(i.Args[1]) ?? QuestOfScript(TypeOf(f, i.Args[1]));
                    if (target == null) { if (a_unresolved != null) a_unresolved.Add(by + ": " + f.Obj + "." + f.Name + ": " + i.Args[0] + " on " + i.Args[1] + (i.Args.Count > 3 ? " " + i.Args[3] : "")); continue; }
                    if (Is(i.Args[0], "SetStage"))
                    {
                        int s;
                        if (i.Args.Count > 3 && int.TryParse(i.Args[3], out s)) add(target.Item2, target.Item1, s, by);
                        else if (a_unresolved != null) a_unresolved.Add(by + ": " + f.Obj + "." + f.Name + ": SetStage on " + target.Item1.Edid + " with " + (i.Args.Count > 3 ? i.Args[3] : "?"));
                    }
                    bool same = own || (quest != null && target.Item1 == quest.Item1);
                    if (!same && (target.Item1.Flags & 1) == 0) foreach (var s in target.Item1.Startup) add(target.Item2, target.Item1, s, by + ", starting " + target.Item1.Edid);
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
                var q = (i.Op == 23 && Own(i.Args[1])) || i.Op == 24 ? quest : i.Op == 23 ? QuestOfScript(type) : null;
                int first = i.Op == 24 ? 2 : 3;
                todo.Enqueue(Tuple.Create(g, i.Args.Skip(first).Select(a => Literal(a) ? a : null).ToArray(), q, by));
            }
        }
        return order;
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
                foreach (var f in funcs) { Script s; if (!scripts.TryGetValue(f.Obj, out s)) continue; s.All.Add(f); if (!s.Funcs.ContainsKey(f.Name)) s.Funcs[f.Name] = f; }
                if (kv.Key.IndexOf(@"\fragments\quests\", StringComparison.OrdinalIgnoreCase) < 0) continue;
                var name = Path.GetFileNameWithoutExtension(kv.Key);
                uint id;
                if (!uint.TryParse(name.Substring(name.LastIndexOf('_') + 1), System.Globalization.NumberStyles.HexNumber, null, out id) || !d.ContainsKey(id & 0xFFFFFF)) continue;
                var key = Tuple.Create(file, id & 0xFFFFFF);
                if (!fragments.ContainsKey(key)) fragments[key] = new Dictionary<int, List<GearScan.Func>>();
                foreach (var f in funcs)
                {
                    int stage;
                    if (!GearScan.StageOf(d[id & 0xFFFFFF], f, out stage)) continue;  // a function no stage binds never runs
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
    // OnQuest: the quest whose record it is attached to (its own scripts, its stage fragments, its aliases' scripts), 0 for others.
    // Owner: the quest it belongs to (a quest's own scripts, its stage fragments' and its aliases'; a topic info's, a
    // scene's and a package's by the record's quest), 0 none; Stages: the quest's stage fragments' script (-NeverSet).
    class Attachment { public Attached Entry; public string Where; public uint Self; public uint SelfQuest; public int SelfAlias = -1; public uint OnQuest; public uint Owner; public bool Stages; }

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
    // -NeverSet: each quest's stages (INDX, the last file's record), its start-up ones and its stage fragments (the
    // fragment script's function, by name, to its stage); and the stages a record sets itself, not a script: a topic
    // info's and a scene's or scene phase's "set parent quest stage" (TIQS, SCQS: on begin, on end).
    // Notes: a stage's log entries' notes (NAM2: the designer's own, never shown in the game), the first of each stage.
    class QuestInfo { public string Edid = ""; public List<int> Stages = new List<int>(); public HashSet<int> Startup = new HashSet<int>(); public Dictionary<string, int> Fragments = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase); public Dictionary<int, string> Notes = new Dictionary<int, string>(); }
    static readonly Dictionary<uint, QuestInfo> quests = new Dictionary<uint, QuestInfo>();
    static readonly List<Tuple<uint, int, string>> recordSets = new List<Tuple<uint, int, string>>();  // quest, stage, the record
    static uint dialQuest;  // the quest of the topic last read (QNAM): its infos follow it
    static readonly List<string> vmadErrorWhere = new List<string>();
    // the scenes that set a stage of their quest as a phase begins or ends (SCQS): where, its PNAMs (its quest's among
    // them), the stages
    static readonly List<Tuple<string, List<uint>, List<int>>> sceneStages = new List<Tuple<string, List<uint>, List<int>>>();
    static readonly List<uint> scenePnams = new List<uint>();
    static readonly List<int> sceneSets = new List<int>();
    static bool recordsRead;

    // A quest's key here (FILES' index in the top byte) from GearScan.MASTERS' file index and its form ID.
    public static uint KeyOf(int a_master, uint a_id)
    {
        return (uint)Array.FindIndex(FILES, f => f.Equals(GearScan.MASTERS[a_master].Esm, StringComparison.OrdinalIgnoreCase)) << 24 | (a_id & 0xFFFFFF);
    }

    // Reads the records of every plugin of FILES found (once), as Run does first: the scripts attached to them and their
    // properties, and the scenes' stages.
    public static void LoadRecords(string a_data)
    {
        if (recordsRead) return;
        recordsRead = true;
        foreach (var file in FILES) { var esm = Path.Combine(a_data, file); if (File.Exists(esm)) Esm(esm, file); }
    }

    // The VMADs that could not be read to their end (what is attached there is not all known), with where.
    public static int VmadErrors(out List<string> a_where) { a_where = vmadErrorWhere; return vmadErrors; }

    // Every script attached anywhere (a record's own, a reference's, a fragment's, an alias's) that holds the form a_key in
    // a property: where, the script, and the names of those properties.
    public static List<Tuple<string, string, List<string>, Dictionary<string, string>>> Holders(uint a_key)
    {
        var list = new List<Tuple<string, string, List<string>, Dictionary<string, string>>>();
        Action<string, Attached> add = (where, s) =>
        {
            if (s == null) return;
            var names = s.Props.Where(p => p.Forms.Contains(a_key)).Select(p => p.Name).ToList();
            if (names.Count > 0) list.Add(Tuple.Create(where, s.Script, names, Literals(s)));
        };
        foreach (var a in attachments) add(a.Where, a.Entry);
        foreach (var kv in baseScripts) foreach (var s in kv.Value.Item2) add(kv.Value.Item1, s);
        foreach (var kv in refScripts) foreach (var s in kv.Value.Item2) add(kv.Value.Item1, s);
        return list;
    }

    // The whole-number values an attached script's properties are given where it is attached, by their variables
    // (::Name_var): what a property not given there holds is not read.
    static Dictionary<string, string> Literals(Attached a_script)
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int n;
        foreach (var p in a_script.Props) if (p.Literal != null && int.TryParse(p.Literal, out n)) d["::" + p.Name + "_var"] = p.Literal;
        return d;
    }

    // The scripts attached to the quest a_key's own record: its own (true: self is the quest), its stage fragments' (true)
    // and its aliases' (false: self is an alias); where and the script.
    public static List<Tuple<string, string, bool, Dictionary<string, string>>> OnQuest(uint a_key)
    {
        return attachments.Where(a => a.OnQuest == a_key).Select(a => Tuple.Create(a.Where, a.Entry.Script, a.SelfAlias < 0, Literals(a.Entry))).ToList();
    }

    // The fragment scripts attached to a topic's info, a scene, a package, a perk or a terminal (not a quest): where and the
    // script. Their GetOwningQuest() is the quest the record belongs to, which is not read here.
    public static List<Tuple<string, string, Dictionary<string, string>>> OtherFragments()
    {
        return attachments.Where(a => a.OnQuest == 0 && a.Where.EndsWith(" (fragments)", StringComparison.Ordinal)).Select(a => Tuple.Create(a.Where, a.Entry.Script, Literals(a.Entry))).ToList();
    }

    // The scenes whose phase sets a stage of the quest a_key (it is one of the scene's PNAMs: its parent quest), each as
    // "where: stages".
    public static List<Tuple<string, List<int>>> SceneStages(uint a_key)
    {
        return sceneStages.Where(t => t.Item2.Contains(a_key)).Select(t => Tuple.Create(t.Item1, t.Item3)).ToList();
    }

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

    // The stage each of a quest's stage fragment functions is bound to by its VMAD's fragment table (function name to
    // stage): what the game runs for a stage, where a Fragment_Stage_ function's name is only the editor's label (the
    // fragment script may hold functions no stage binds, as V81_03's 1, 200 and 300). Null if the VMAD can't be read.
    public static Dictionary<string, int> QuestFragmentStages(byte[] a_body, int a_at, int a_size)
    {
        var stages = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        try
        {
            var v = new V { B = a_body, P = a_at, End = a_at + a_size };
            v.S16(); v.Format = v.S16();
            Scripts(v);
            if (v.P < v.End)
            {
                v.U8();
                int frags = v.U16();
                ReadScript(v);
                for (int k = 0; k < frags; k++) { int stage = v.U16(); v.S16(); v.U32(); v.U8(); v.W(); var function = v.W(); stages[function] = stage; }
            }
        }
        catch (Exception) { return null; }
        return stages;
    }

    // A record's VMAD: its scripts, and a quest's, a topic's, a scene's, a package's, a perk's or a terminal's fragment
    // script, and a quest's aliases' scripts. Every one is read to its end, but for the fragments' names.
    static void Vmad(string a_type, uint a_key, byte[] a_body, int a_at, int a_size, List<string> a_masters, string a_file, string a_where, uint a_owner)
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
                    if (e != null) found.Add(new Attachment { Entry = e, Where = a_where + " (stage fragments)", OnQuest = a_key, Owner = a_owner, Stages = true });
                    QuestInfo info;
                    quests.TryGetValue(a_key, out info);
                    for (int k = 0; k < frags; k++) { int stage = v.U16(); v.S16(); v.U32(); v.U8(); v.W(); var function = v.W(); if (info != null) info.Fragments[function] = stage; }
                    int aliases = v.U16();
                    for (int k = 0; k < aliases; k++)
                    {
                        uint form; int alias; v.Obj(out form, out alias);
                        v.S16(); v.Format = v.S16();
                        foreach (var s in Scripts(v)) found.Add(new Attachment { Entry = s, Where = a_where + " alias " + alias, SelfQuest = a_key, SelfAlias = alias, OnQuest = a_key, Owner = a_owner });
                    }
                }
                else
                {
                    if (a_type == "INFO" || a_type == "PACK" || a_type == "SCEN") v.U8();  // which fragments there are
                    var e = ReadScript(v);
                    if (e != null) found.Add(new Attachment { Entry = e, Where = a_where + " (fragments)", Owner = a_owner });
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
        else if (FRAGMENTS.Contains(a_type)) foreach (var s in own) attachments.Add(new Attachment { Entry = s, Where = a_where, OnQuest = a_type == "QUST" ? a_key : 0, Owner = a_owner });
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
            QuestInfo quest = null;
            if (type == "QUST") quests[key] = quest = new QuestInfo();  // a later file's record replaces it
            uint owner = type == "QUST" ? key : type == "INFO" ? dialQuest : 0;
            var sets = new List<int>();
            int noteStage = -1;  // the stage whose log entries are being read (a quest's stages come before its objectives and aliases)
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
                else if (type == "SCEN" && st == "PNAM" && ss >= 4)
                {
                    scenePnams.Add(Key(BitConverter.ToUInt32(body, d), masters, a_file));
                    if (ss == 4) owner = Key(BitConverter.ToUInt32(body, d), masters, a_file);  // a scene's quest is its last PNAM
                }
                else if (type == "SCEN" && st == "SCQS")
                {
                    for (int k = 0; k + 2 <= ss; k += 2) sceneSets.Add(BitConverter.ToInt16(body, d + k));
                    if (ss >= 4) { sets.Add(BitConverter.ToInt16(body, d)); sets.Add(BitConverter.ToInt16(body, d + 2)); }
                }
                else if (type == "QUST" && st == "INDX" && ss >= 2)
                {
                    quest.Stages.Add(BitConverter.ToUInt16(body, d));
                    if (ss >= 3 && (body[d + 2] & 0x02) != 0) quest.Startup.Add(BitConverter.ToUInt16(body, d));
                    noteStage = BitConverter.ToUInt16(body, d);
                }
                else if (type == "QUST" && (st == "QOBJ" || st == "ANAM" || st == "ALST" || st == "ALLS")) noteStage = -1;
                else if (type == "QUST" && st == "NAM2" && noteStage >= 0 && ss > 1 && !quest.Notes.ContainsKey(noteStage))
                    quest.Notes[noteStage] = Encoding.ASCII.GetString(body, d, ss - 1).Replace('\r', ' ').Replace('\n', ' ').Trim();
                else if (type == "DIAL" && st == "QNAM" && ss >= 4) dialQuest = Key(BitConverter.ToUInt32(body, d), masters, a_file);
                // a scene's quest is its last PNAM (its actions' packages are PNAMs too, before it: the SCEN branch above); a package's its QNAM
                else if (type == "PACK" && st == "QNAM" && ss == 4) owner = Key(BitConverter.ToUInt32(body, d), masters, a_file);
                else if (type == "INFO" && st == "TIQS" && ss >= 4) { sets.Add(BitConverter.ToInt16(body, d)); sets.Add(BitConverter.ToInt16(body, d + 2)); }
                s = d + ss;
            }
            if (type == "SCEN" && sceneSets.Count > 0) sceneStages.Add(Tuple.Create("SCEN " + edid, new List<uint>(scenePnams), new List<int>(sceneSets)));
            scenePnams.Clear(); sceneSets.Clear();
            if (placed) r.Edid = edid;
            if (type == "NPC_" && edid.Length > 0) npcEdids[key] = edid;
            if (quest != null) quest.Edid = edid;
            foreach (var stage in sets) if (stage >= 0) recordSets.Add(Tuple.Create(owner, stage, type + " " + (edid.Length > 0 ? edid : key.ToString("X8"))));
            if (vmadAt >= 0) Vmad(type, key, body, vmadAt, vmadSize, masters, a_file, type + " " + (edid.Length > 0 ? edid : key.ToString("X8")), owner);
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

    // The NPC bases' editor IDs, by form key, for Actors.
    static readonly Dictionary<uint, string> npcEdids = new Dictionary<uint, string>();

    // The placed actors (ACHR) of the given NPC bases (by editor ID), read from Fallout4.esm: "<base> <ref form ID>", one per
    // line (-Actors NickValentine,CompanionCait: the companions' references StageRules.h's DRIVERS name).
    public static string Actors(string a_data, string[] a_bases)
    {
        Esm(Path.Combine(a_data, FILES[0]), FILES[0]);
        var wanted = new HashSet<string>(a_bases ?? new string[0], StringComparer.OrdinalIgnoreCase);
        var sb = new StringBuilder();
        foreach (var kv in refs.OrderBy(kv => kv.Key))
        {
            string edid;
            if (kv.Value.Actor && npcEdids.TryGetValue(kv.Value.Base, out edid) && wanted.Contains(edid))
                sb.AppendLine(string.Format("{0} {1:X8}{2}", edid, kv.Key, kv.Value.Edid.Length > 0 ? " " + kv.Value.Edid : ""));
        }
        return sb.Length > 0 ? sb.ToString() : "# none found";
    }

    // Reads every file's records and scripts.
    static void LoadFiles(string a_data, StringBuilder report, List<string> scanned)
    {
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
    }

    // A reference's scripts: its base's, with the properties the reference sets over them, and its own.
    static void MergeRefs()
    {
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
    }

    public static Result Run(string a_data)
    {
        var report = new StringBuilder();
        var scanned = new List<string>();
        LoadFiles(a_data, report, scanned);
        foreach (var s in scripts.Values) foreach (var f in s.Funcs) { summaries[f] = new HashSet<int>(); foreach (var i in f.Code) if (i.Op == 29 && i.Args.Count > 0) setByAnyone.Add(i.Args[0]); }
        for (int pass = 0; pass < 12; pass++)
        {
            bool changed = false;
            var scratch = new Sink();
            foreach (var s in scripts.Values) foreach (var f in s.Funcs) changed |= Analyze(f, null, scratch, summaries[f]);
            if (!changed) { report.AppendLine("# calls followed in " + (pass + 1) + " passes"); break; }
        }

        MergeRefs();

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

    // ---------------- -NeverSet ----------------
    // The stages nothing the scan can see ever sets from play: what RuleScan's DEBUG (the developers' quickstarts, named
    // by hand) is checked against, so that a quest's quickstarts are not missed again. A stage is set from play by:
    //  - its quest starting, for a start-up stage;
    //  - a topic info or a scene (or one of its phases) whose record sets its quest's stage (TIQS, SCQS: recordSets);
    //  - a SetStage call that an entry point of an attached script reaches, itself or through the functions it calls,
    //    on a quest and with a stage the scan can name. An entry point is what the game calls by itself: an event (On...,
    //    a remote or custom event's handler), a fragment (a topic info's, a scene's, a package's, a perk's, a terminal's)
    //    or a function called by name or on an object whose script is not known. A function nothing calls sets nothing:
    //    only a console's cqf runs it. The scripts are those of every record the files hold (ParentScan's attachments,
    //    and each base object's as the base has them), each with the values its properties have there;
    //  - a stage's own fragment, which counts only if that stage is set from play itself: the stages left are the ones
    //    whose only setters are each other (the greatest such set). Those are a console's.
    // The quest of a SetStage is read from where its object came from: the script's own quest (self in a quest's script
    // or fragment, GetOwningQuest of an alias, a topic info's or a scene's quest), a property (the form the attachment
    // gives it; an alias property's quest), another script's property (every attachment of that script), a variable the
    // script writes (what it writes there), a parameter (what each caller passes). The stage likewise: a literal, a
    // property's value there or its default in the script, a variable, a parameter. What it cannot name (a call's
    // result, an array of numbers, a struct's member, arithmetic, an event's own argument) is counted, not guessed:
    // such a call could set any stage, and a quest that has one is marked in the list.
    // Not seen at all: a SetStage that the game's code makes (none is known), and a script attached at run time.
    static readonly Dictionary<GearScan.Func, HashSet<string>> stageSinks = new Dictionary<GearScan.Func, HashSet<string>>();  // "quest origin|stage origin"
    static readonly Dictionary<string, HashSet<string>> stageWrites = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);  // "script|variable" -> origins
    static readonly HashSet<string> looseCalls = new HashSet<string>(StringComparer.OrdinalIgnoreCase);  // functions called by name, or on an object of no known script
    static readonly Dictionary<string, List<GearScan.Func>> calleesOf = new Dictionary<string, List<GearScan.Func>>(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, List<Attachment>> attachedAs = new Dictionary<string, List<Attachment>>(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, Tuple<HashSet<uint>, bool>> foreignQuests = new Dictionary<string, Tuple<HashSet<uint>, bool>>(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, Tuple<HashSet<int>, bool>> foreignStages = new Dictionary<string, Tuple<HashSet<int>, bool>>(StringComparer.OrdinalIgnoreCase);
    static readonly HashSet<int> NO_WRITE = new HashSet<int> { 0, 20, 21, 22, 26, 29, 33, 39 };

    // the functions a call by name on a script runs: the first script up its parents that has one (every state's)
    static List<GearScan.Func> Callees(string a_type, string a_name)
    {
        List<GearScan.Func> list;
        string key = (a_type ?? "") + "|" + a_name;
        if (calleesOf.TryGetValue(key, out list)) return list;
        list = new List<GearScan.Func>();
        if (a_type != null)
            foreach (var s in Chain(a_type)) { list = s.Funcs.Where(x => Is(x.Name, a_name)).ToList(); if (list.Count > 0) break; }
        calleesOf[key] = list;
        return list;
    }

    // a value written to a script's variable or property: kept as it is when it means the same anywhere (a number,
    // another script's property) or is the script's own (a_own: its property, itself), else not known
    static void Written(string a_script, string a_name, IEnumerable<string> a_from, bool a_own)
    {
        HashSet<string> set;
        string key = a_script + "|" + a_name;
        if (!stageWrites.TryGetValue(key, out set)) stageWrites[key] = set = new HashSet<string>();
        foreach (var o in a_from) set.Add(o[0] == '#' || o[0] == 'F' || (a_own && (o[0] == 'P' || o[0] == 'S')) ? o : "?");
    }

    // One function's SetStage calls, its own and those of the functions it calls, as "quest|stage" by where each came
    // from: "S" the script's own object, "A<k>" its k-th parameter, "P<name>" a property or variable of the script,
    // "F<script>.<name>" another script's property, "#<n>" a number, "?" not known. In the order of the code, twice (a
    // local keeps every value written to it, a temporary the last). True: it found a call it had not.
    static bool StageFlow(GearScan.Func f)
    {
        var sinks = stageSinks[f];
        int before = sinks.Count;
        var cur = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
        for (int k = 0; k < f.Params.Count; k++) cur[f.Params[k]] = new HashSet<string> { "A" + k };
        Func<string, HashSet<string>> origin = a =>
        {
            var set = new HashSet<string>();
            double n;
            HashSet<string> known;
            if (Is(a, "self")) set.Add("S");
            else if (a == "None" || Is(a, "::nonevar")) { }
            else if (Number(a, out n)) set.Add(n == Math.Floor(n) && Math.Abs(n) < 100000 ? "#" + (int)n : "?");
            else if (Literal(a)) set.Add("?");
            else if (!f.Types.ContainsKey(a)) set.Add("P" + Bare(a));
            else if (cur.TryGetValue(a, out known)) set.UnionWith(known);
            else set.Add("?");
            return set;
        };
        Action<string, IEnumerable<string>> write = (dst, from) =>
        {
            if (Is(dst, "::nonevar")) return;
            if (!f.Types.ContainsKey(dst)) { Written(f.Obj, Bare(dst), from, true); return; }
            HashSet<string> set;
            if (dst.StartsWith("::") || !cur.TryGetValue(dst, out set)) cur[dst] = new HashSet<string>(from);
            else set.UnionWith(from);
        };
        Action<string, string> add = (q, s) => { if (sinks.Count < 300) sinks.Add(q + "|" + s); else sinks.Add("?|?"); };
        var unknown = new[] { "?" };
        for (int pass = 0; pass < 2; pass++)
            foreach (var i in f.Code)
            {
                var a = i.Args;
                if ((i.Op == 13 || i.Op == 14 || i.Op == 32) && a.Count >= 2) write(a[0], origin(a[1]));  // 32: an array's element, any of the array's
                else if (i.Op == 28 && a.Count >= 3)  // propget name, object, dest
                {
                    var t = TypeOf(f, a[1]);
                    write(a[2], Is(a[1], "self") ? new[] { "P" + a[0] } : t != null ? new[] { "F" + t + "." + a[0] } : unknown);
                }
                else if (i.Op == 29 && a.Count >= 3)  // propset name, object, value
                {
                    var t = Is(a[1], "self") ? f.Obj : TypeOf(f, a[1]);
                    if (pass == 1 && t != null) Written(t, a[0], origin(a[2]), Is(a[1], "self"));
                }
                else if ((i.Op == 23 || i.Op == 24 || i.Op == 25) && a.Count >= 3)
                {
                    // callmethod name, object, dest, arguments; callparent name, dest, arguments; callstatic script, name, dest, arguments
                    string name = i.Op == 25 ? a[1] : a[0];
                    int first = i.Op == 24 ? 2 : 3;
                    string dest = i.Op == 24 ? a[1] : a[2];
                    string type = i.Op == 23 ? TypeOf(f, a[1]) : i.Op == 25 ? a[0] : Chain(f.Obj).Select(s => s.Obj.Parent).FirstOrDefault();
                    var obj = i.Op == 23 ? origin(a[1]) : i.Op == 24 ? new HashSet<string> { "S" } : new HashSet<string> { "?" };
                    bool own = i.Op == 24 || (i.Op == 23 && Is(a[1], "self"));
                    IEnumerable<string> result = unknown;
                    var callees = Callees(type, name);
                    // the game's own setter (Quest.SetStage is a script function that calls it), or SetStage on an object of no known script
                    if (i.Op == 23 && a.Count > first && (Is(name, "SetCurrentStageID") || (Is(name, "SetStage") && callees.Count == 0)))
                    {
                        if (pass == 1) foreach (var q in obj) foreach (var s in origin(a[first])) add(q, s);
                    }
                    else if (i.Op == 23 && Is(name, "GetOwningQuest")) result = obj;
                    else if (pass == 1)
                    {
                        if (i.Op == 23 && type == null) looseCalls.Add(name);
                        if ((Is(name, "CallFunction") || Is(name, "CallFunctionNoWait")) && a.Count > first && a[first].StartsWith("\"")) looseCalls.Add(a[first].Trim('"'));
                        if (Is(name, "CallGlobalFunction") && a.Count > first + 1 && a[first + 1].StartsWith("\"")) looseCalls.Add(a[first + 1].Trim('"'));
                        foreach (var c in callees)
                        {
                            if (c == f) continue;
                            foreach (var sink in stageSinks[c].ToList())
                            {
                                int bar = sink.IndexOf('|');
                                Func<string, IEnumerable<string>> map = o =>
                                {
                                    if (o == "S") return obj;
                                    if (o[0] == 'A') { int k = first + int.Parse(o.Substring(1)); return k < a.Count ? origin(a[k]) : new HashSet<string> { "?" }; }
                                    if (o[0] == 'P' && !own) return new[] { "F" + c.Obj + "." + o.Substring(1) };
                                    return new[] { o };
                                };
                                foreach (var q in map(sink.Substring(0, bar)).ToList()) foreach (var s in map(sink.Substring(bar + 1)).ToList()) add(q, s);
                            }
                        }
                    }
                    write(dest, result);
                }
                else if (!NO_WRITE.Contains(i.Op) && a.Count >= 2)
                    write(a[i.Op == 34 || i.Op == 35 || i.Op == 40 || i.Op == 41 ? 1 : 0], unknown);
            }
        return sinks.Count != before;
    }

    static bool EntryPoint(GearScan.Func f)
    {
        return f.Name.StartsWith("On", StringComparison.OrdinalIgnoreCase) || f.Name.StartsWith("::remote", StringComparison.OrdinalIgnoreCase)
            || f.Name.StartsWith("Fragment_", StringComparison.OrdinalIgnoreCase) || looseCalls.Contains(f.Name);
    }

    // The quests an origin can be at one attachment, added to a_into; true: or one the scan cannot name.
    static bool QuestsOf(Attachment a_at, string a_origin, int a_depth, HashSet<uint> a_into)
    {
        if (a_depth > 3) return true;
        if (a_origin == "S")
        {
            if (a_at.Owner == 0 || !quests.ContainsKey(a_at.Owner)) return true;
            a_into.Add(a_at.Owner);
            return false;
        }
        if (a_origin[0] == 'P')
        {
            bool unknown = false;
            string name = a_origin.Substring(1);
            foreach (var p in a_at.Entry.Props) if (Is(p.Name, name)) foreach (var form in p.Forms) if (quests.ContainsKey(form)) a_into.Add(form);
            foreach (var s in Chain(a_at.Entry.Script))
            {
                HashSet<string> written;
                if (!stageWrites.TryGetValue(s.Obj.Name + "|" + name, out written)) continue;
                foreach (var w in written) if (!Is(w, a_origin)) unknown |= QuestsOf(a_at, w, a_depth + 1, a_into);
            }
            return unknown;
        }
        if (a_origin[0] == 'F')
        {
            Tuple<HashSet<uint>, bool> known;
            if (!foreignQuests.TryGetValue(a_origin, out known))
            {
                foreignQuests[a_origin] = Tuple.Create(new HashSet<uint>(), true);  // while it is being found: not known
                int dot = a_origin.LastIndexOf('.');
                var set = new HashSet<uint>();
                bool unknown = false;
                List<Attachment> list;
                if (attachedAs.TryGetValue(a_origin.Substring(1, dot - 1), out list))
                    foreach (var other in list) unknown |= QuestsOf(other, "P" + a_origin.Substring(dot + 1), a_depth + 1, set);
                foreignQuests[a_origin] = known = Tuple.Create(set, unknown);
            }
            a_into.UnionWith(known.Item1);
            return known.Item2;
        }
        return true;  // "?", a parameter nothing fills, a number
    }

    // The stages an origin can be at one attachment, added to a_into; true: or one the scan cannot name.
    static bool StagesOf(Attachment a_at, string a_origin, int a_depth, HashSet<int> a_into)
    {
        if (a_depth > 3) return true;
        if (a_origin[0] == '#') { a_into.Add(int.Parse(a_origin.Substring(1))); return false; }
        if (a_origin[0] == 'P')
        {
            bool unknown = false, given = false;
            string name = a_origin.Substring(1);
            double n;
            foreach (var p in a_at.Entry.Props)
            {
                if (!Is(p.Name, name)) continue;
                given = true;
                if (p.Literal != null && Number(p.Literal, out n) && n == Math.Floor(n)) a_into.Add((int)n); else unknown = true;  // an array, a struct, a form
            }
            foreach (var s in Chain(a_at.Entry.Script))
            {
                if (!given)
                    foreach (var d in s.Obj.Defaults)
                        if (Is(Bare(d.Key), name)) { given = true; if (Number(d.Value, out n) && n == Math.Floor(n)) a_into.Add((int)n); else if (d.Value != "None") unknown = true; }
                HashSet<string> written;
                if (!stageWrites.TryGetValue(s.Obj.Name + "|" + name, out written)) continue;
                foreach (var w in written) if (!Is(w, a_origin)) unknown |= StagesOf(a_at, w, a_depth + 1, a_into);
            }
            return unknown;  // no value there, none in the script and never written: no stage (a property left empty)
        }
        if (a_origin[0] == 'F')
        {
            Tuple<HashSet<int>, bool> known;
            if (!foreignStages.TryGetValue(a_origin, out known))
            {
                foreignStages[a_origin] = Tuple.Create(new HashSet<int>(), true);
                int dot = a_origin.LastIndexOf('.');
                var set = new HashSet<int>();
                bool unknown = false;
                List<Attachment> list;
                if (attachedAs.TryGetValue(a_origin.Substring(1, dot - 1), out list))
                    foreach (var other in list) unknown |= StagesOf(other, "P" + a_origin.Substring(dot + 1), a_depth + 1, set);
                foreignStages[a_origin] = known = Tuple.Create(set, unknown);
            }
            a_into.UnionWith(known.Item1);
            return known.Item2;
        }
        return true;
    }

    class StageSetters
    {
        public int Real; public List<string> Examples = new List<string>();  // set from play: how often, and where (the first few)
        public HashSet<long> Stages = new HashSet<long>();                    // the stages whose fragment sets it
        public void From(string a_where) { Real++; if (Examples.Count < 6 && !Examples.Contains(a_where)) Examples.Add(a_where); }
    }
    static long StageKey(uint a_quest, int a_stage) { return ((long)a_quest << 16) | (long)(uint)(a_stage & 0xFFFF); }

    // a_show: a regular expression on a quest's editor ID; each stage of a matching quest is listed with what sets it.
    public static string NeverSet(string a_data, string a_show)
    {
        var report = new StringBuilder();
        var scanned = new List<string>();
        LoadFiles(a_data, report, scanned);
        MergeRefs();
        foreach (var kv in baseScripts) foreach (var s in kv.Value.Item2) attachments.Add(new Attachment { Entry = s, Where = kv.Value.Item1 });

        var all = scripts.Values.SelectMany(s => s.Funcs).ToList();
        foreach (var f in all) stageSinks[f] = new HashSet<string>();
        int passes = 1;
        for (; passes <= 16; passes++)
        {
            bool changed = false;
            foreach (var f in all) changed |= StageFlow(f);
            if (!changed) break;
        }
        foreach (var a in attachments)
            foreach (var s in Chain(a.Entry.Script))
            {
                List<Attachment> list;
                if (!attachedAs.TryGetValue(s.Obj.Name, out list)) attachedAs[s.Obj.Name] = list = new List<Attachment>();
                list.Add(a);
            }

        var setters = new Dictionary<long, StageSetters>();
        Func<uint, int, StageSetters> at = (q, s) =>
        {
            StageSetters set;
            if (!setters.TryGetValue(StageKey(q, s), out set)) setters[StageKey(q, s)] = set = new StageSetters();
            return set;
        };
        int recordsNoQuest = 0, orphans = 0, calls = 0, blindBoth = 0, blindQuestCalls = 0;
        var blindStage = new Dictionary<uint, List<string>>();  // a quest's SetStage with a stage not known: where
        var blindQuest = new Dictionary<int, int>();            // a stage set on a quest not known: how often
        var blindBothWhere = new List<string>();
        foreach (var r in recordSets)
        {
            if (quests.ContainsKey(r.Item1)) at(r.Item1, r.Item2).From(r.Item3); else recordsNoQuest++;
        }
        var entries = new Dictionary<string, List<GearScan.Func>>(StringComparer.OrdinalIgnoreCase);
        foreach (var a in attachments)
        {
            List<GearScan.Func> list;
            if (!entries.TryGetValue(a.Entry.Script, out list))
            {
                entries[a.Entry.Script] = list = new List<GearScan.Func>();
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var s in Chain(a.Entry.Script))
                {
                    foreach (var f in s.Funcs) if (!seen.Contains(f.Name) && stageSinks[f].Count > 0 && EntryPoint(f)) list.Add(f);
                    foreach (var f in s.Funcs) seen.Add(f.Name);
                }
            }
            if (list.Count == 0) continue;
            QuestInfo own = null;
            if (a.Stages) quests.TryGetValue(a.Owner, out own);
            foreach (var f in list)
            {
                int from = -1;
                bool byStage = own != null && own.Fragments.TryGetValue(f.Name, out from);
                if (own != null && !byStage && f.Name.StartsWith("Fragment_", StringComparison.OrdinalIgnoreCase)) { orphans++; continue; }  // a fragment no stage runs
                string where = a.Where + ": " + f.Obj + "." + f.Name;
                foreach (var sink in stageSinks[f])
                {
                    int bar = sink.IndexOf('|');
                    var qs = new HashSet<uint>();
                    var ss = new HashSet<int>();
                    bool anyQuest = QuestsOf(a, sink.Substring(0, bar), 0, qs);
                    bool anyStage = StagesOf(a, sink.Substring(bar + 1), 0, ss);
                    calls++;
                    foreach (var q in qs)
                    {
                        foreach (var s in ss) { if (byStage) at(q, s).Stages.Add(StageKey(a.Owner, from)); else at(q, s).From(where); }
                        if (anyStage) { List<string> l; if (!blindStage.TryGetValue(q, out l)) blindStage[q] = l = new List<string>(); if (l.Count < 4 && !l.Contains(where)) l.Add(where); }
                    }
                    if (anyQuest) { blindQuestCalls++; foreach (var s in ss) blindQuest[s] = (blindQuest.ContainsKey(s) ? blindQuest[s] : 0) + 1; }
                    if (anyQuest && anyStage) { blindBoth++; if (blindBothWhere.Count < 400 && !blindBothWhere.Contains(where)) blindBothWhere.Add(where); }
                }
            }
        }

        // the stages with no setter from play, less those a stage that is set from play sets: the rest set each other only
        var candidates = new HashSet<long>();
        foreach (var kv in quests)
            foreach (var stage in kv.Value.Stages.Distinct())
            {
                StageSetters set;
                if (kv.Value.Startup.Contains(stage) || (setters.TryGetValue(StageKey(kv.Key, stage), out set) && set.Real > 0)) continue;
                candidates.Add(StageKey(kv.Key, stage));
            }
        for (bool changed = true; changed;)
        {
            changed = false;
            foreach (var c in candidates.ToList())
            {
                StageSetters set;
                if (setters.TryGetValue(c, out set) && set.Stages.Any(s => s != c && !candidates.Contains(s))) { candidates.Remove(c); changed = true; }
            }
        }

        MoveScan.Load(a_data);
        var master = FILES.Select(f => Array.FindIndex(GearScan.MASTERS, m => Is(m.Esm, f))).ToArray();
        Func<uint, Tuple<int, GearScan.Quest>> game = key =>
        {
            int file = master[key >> 24];
            GearScan.Quest q = null;
            if (file >= 0) MoveScan.perFile[file].TryGetValue(key & 0xFFFFFF, out q);
            return Tuple.Create(file, q);
        };
        Func<long, string> name = key =>
        {
            QuestInfo info;
            uint quest = (uint)(key >> 16);
            return (quests.TryGetValue(quest, out info) ? info.Edid : quest.ToString("X8")) + " " + (key & 0xFFFF);
        };
        var show = string.IsNullOrEmpty(a_show) ? null : new System.Text.RegularExpressions.Regex(a_show, System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        var lines = new List<string>();
        var summary = new List<string>();
        var missed = new List<string>();
        int listed = 0, listedQuests = 0, notShared = 0, noFragment = 0, otherFile = 0, marked = 0;
        foreach (var kv in quests.OrderBy(k => master[k.Key >> 24] < 0 ? 99 : master[k.Key >> 24]).ThenBy(k => k.Key & 0xFFFFFF))
        {
            var info = kv.Value;
            var g = game(kv.Key);
            var stages = info.Stages.Distinct().OrderBy(s => s).ToList();
            if (show != null && show.IsMatch(info.Edid))
            {
                lines.Add(string.Format("# {0} ({1:X8}), {2}:", info.Edid, kv.Key, g.Item2 == null ? "not in the masters" : QuestScan.Shared(g.Item2, g.Item1)));
                foreach (var stage in stages)
                {
                    StageSetters set;
                    setters.TryGetValue(StageKey(kv.Key, stage), out set);
                    lines.Add(string.Format("#   {0}{1}{2}{3}: {4}", stage, info.Startup.Contains(stage) ? " (start-up)" : "", info.Fragments.ContainsValue(stage) ? "" : " (no fragment)",
                        candidates.Contains(StageKey(kv.Key, stage)) ? " NEVER SET" : "",
                        set == null ? "no setter" : (set.Real > 0 ? set.Real + " from play (" + string.Join("; ", set.Examples.ToArray()) + ")" : "none from play")
                            + (set.Stages.Count > 0 ? "; stages " + string.Join(", ", set.Stages.OrderBy(s => s).Select(s => name(s) + (candidates.Contains(s) ? "*" : "")).ToArray()) : "")
                            + (info.Notes.ContainsKey(stage) ? "; note \"" + info.Notes[stage] + "\"" : "")));
                }
                List<string> blind;
                if (blindStage.TryGetValue(kv.Key, out blind)) lines.Add("#   a stage the scan cannot name is set on it by: " + string.Join("; ", blind.ToArray()));
                continue;
            }
            if (g.Item2 != null)
                foreach (var stage in stages)
                    if (RuleScan.IsDebug(info.Edid, stage))
                    {
                        marked++;
                        if (!candidates.Contains(StageKey(kv.Key, stage))) missed.Add(info.Edid + " " + stage);
                    }
            if (show != null) continue;
            var mine = stages.Where(s => candidates.Contains(StageKey(kv.Key, s))).ToList();
            if (mine.Count == 0) continue;
            if (g.Item2 == null) { otherFile += mine.Count; continue; }
            if (!QuestScan.ReachesFriend(g.Item2, g.Item1)) { notShared += mine.Count; continue; }
            var withFragment = mine.Where(s => info.Fragments.ContainsValue(s)).ToList();
            noFragment += mine.Count - withFragment.Count;
            if (withFragment.Count == 0) continue;
            listedQuests++;
            int moves = 0, hostile = 0, dismisses = 0, sets = 0;
            foreach (var stage in withFragment)
            {
                listed++;
                var what = new List<string>();
                var moved = MoveScan.StageMoves(g.Item1, g.Item2, stage);
                if (moved != null) { moves++; what.Add("moves the player (" + moved.Substring(moved.LastIndexOf(": ", StringComparison.Ordinal) + 2) + ")"); }
                var turned = SafeScan.StageHostile(g.Item1, g.Item2, stage);
                if (turned != null) { hostile++; what.Add("turns a faction (" + SafeScan.HostileFaction(turned) + ")"); }
                if (SafeScan.StageDismisses(g.Item1, g.Item2, stage) != null) { dismisses++; what.Add("dismisses a companion"); }
                var open = new List<string>();
                int shared = MoveScan.Sets(g.Item1, g.Item2, stage, false, open).Count(r => !(r.Quest == g.Item2 && r.Stage == stage) && QuestScan.ReachesFriend(r.Quest, r.File));
                if (shared > 0) { sets++; what.Add("sets " + shared + " shared stage" + (shared > 1 ? "s" : "")); }
                StageSetters set;
                setters.TryGetValue(StageKey(kv.Key, stage), out set);
                var by = set == null ? new List<string>() : set.Stages.Where(s => s != StageKey(kv.Key, stage)).OrderBy(s => s).Select(s => name(s)).ToList();
                lines.Add(string.Format("{0} {1:X6} {2} {3}{4}: {5}{6}{7}", g.Item1, kv.Key & 0xFFFFFF, info.Edid, stage, RuleScan.IsDebug(info.Edid, stage) ? " [kDebug]" : "",
                    what.Count > 0 ? string.Join("; ", what.ToArray()) : "nothing of those", by.Count > 0 ? "; set by " + string.Join(", ", by.ToArray()) : "",
                    info.Notes.ContainsKey(stage) ? "; note \"" + info.Notes[stage] + "\"" : ""));
            }
            summary.Add(string.Format("{0} {1:X6} {2} ({3}): {4}{5}{6}{7}{8}{9}{10}", g.Item1, kv.Key & 0xFFFFFF, info.Edid, QuestScan.Shared(g.Item2, g.Item1),
                string.Join(", ", withFragment.Select(s => s.ToString()).ToArray()),
                withFragment.All(s => RuleScan.IsDebug(info.Edid, s)) ? " [all kDebug]" : withFragment.Any(s => RuleScan.IsDebug(info.Edid, s)) ? " [some kDebug]" : "",
                moves > 0 ? "; " + moves + " move the player" : "", hostile > 0 ? "; " + hostile + " turn a faction" : "", dismisses > 0 ? "; " + dismisses + " dismiss a companion" : "",
                sets > 0 ? "; " + sets + " set shared stages" : "", blindStage.ContainsKey(kv.Key) ? "; a stage the scan cannot name is set on it (" + blindStage[kv.Key][0] + ")" : ""));
        }
        var sb = new StringBuilder();
        foreach (var l in lines) sb.AppendLine(l);
        if (show != null) return sb.ToString();
        sb.AppendLine("# by quest:");
        foreach (var l in summary) sb.AppendLine("#   " + l);
        sb.AppendLine(string.Format("# {0} stages of {1} quests listed: never set from play (see ParentScan's -NeverSet in tools/questscan.cs), of a quest a friend's stage is set on, with a fragment", listed, listedQuests));
        sb.AppendLine(string.Format("# not listed: {0} with no fragment (nothing to run); {1} of quests no friend's stage is set on; {2} of quests of a file that is not a master", noFragment, notShared, otherFile));
        sb.AppendLine(string.Format("# DEBUG's quickstarts (kDebug): {0}, of them not found never set: {1}", marked, missed.Count == 0 ? "none" : string.Join(", ", missed.ToArray())));
        sb.AppendLine(string.Format("# read: {0} quests, {1} stages set by a topic info's or a scene's record ({2} of no known quest), {3} SetStage calls reached from {4} script attachments in {5} passes",
            quests.Count, recordSets.Count, recordsNoQuest, calls, attachments.Count, passes));
        sb.AppendLine(string.Format("# not named: {0} calls on a quest the scan cannot name ({1} of them with a stage it cannot name either), {2} quests with a call whose stage it cannot name; {3} stage fragments no stage runs left out",
            blindQuestCalls, blindBoth, blindStage.Count, orphans));
        sb.AppendLine("# the calls whose quest and stage are both not named:");
        foreach (var w in blindBothWhere.Select(w => w.Substring(w.IndexOf(": ", StringComparison.Ordinal) + 2)).GroupBy(w => w).OrderByDescending(x => x.Count()).Take(60))
            sb.AppendLine("#   " + w.Key + (w.Count() > 1 ? " x" + w.Count() : ""));
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
// StageRules::Starting judges every stage that starts the quest (as MQ203's and MQ204's 0); one that DEBUG names has
// its kDebug row all the same. Bridges (kBridge) and the
// story (kStory) are not decided here; a developer's quickstart is, by hand (DEBUG: kDebug). The Automatron and
// Nuka-World rows were made with it; the older rows of Fallout4.esm and DLCCoast.esm were made before it, their payments
// traced by hand, and differ from its output in their waits.
public static class RuleScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }

    // The developers' quickstarts among the shared quests' stages: stages only a console's setstage sets (no fragment,
    // script or other quest sets them from play: -SetsOther finds no setter but each other), each of which puts the
    // player at a point of the quest and sets the story up to it, a faction's kick-out included. Read by hand in the
    // fragments. Their rows get kDebug (StageRules.h: never shared, neither reported nor set from a friend's), and a
    // row even when the stage neither moves, takes nor waits, or is a stage 0 that is no start-up stage (the other
    // quests' stage 0s of this kind are left out by the start-up rule above: no row). Each prints with "[a
    // quickstart]". What each sets in the game it is typed in is QuickstartScan's (-Quickstarts: QUICKSTART_SETS), so
    // that isn't sent either (a row for each quickstart stage). -NeverSet lists
    // every stage that nothing the scan can see sets from play, with the designers' notes: the list this one is checked
    // against, and where the next quickstart to add is found.
    class Debug { public string Quest; public int From; public int To; public string Why; }
    static readonly Debug[] DEBUG = {
        new Debug { Quest = "DN084", From = 1, To = 9, Why = "Mass Fusion's quickstarts: each moves the player to a DN084_...QuickstartMarker (the lobby, the descent, the reactor, the battle, on the Brotherhood's path or the Institute's) and sets that path's stages up to it: 2, 4, 6 and 8 InstMassFusion's or BoSKickOut 30 (the Brotherhood's kick-out), 1, 3, 5, 7 and 9 BoS303's, its 30 the Institute's kick-out. In play the quest starts at 10" },
        new Debug { Quest = "BoS303", From = 0, To = 3, Why = "Mass Fusion's quickstarts on the Brotherhood's side, in its own quest: each puts the player in the Brotherhood, sets its questline done up to here (BoS100 to BoS302B 255) and moves them to a marker (0 BoS303QSMarker and 3 BoS303Start on the Prydwen, 1 DN084_BoS303QuickstartMarker at Mass Fusion, 2 through DN084 3); 1 sets its own 30 (the Institute the player's enemy, and its kick-out for a player in it), 2 sets DN084 3, which does. Nothing else sets them and the quest has no start-up stage" },
        new Debug { Quest = "Inst307Fight", From = 0, To = 8, Why = "the airport battle's quickstarts: 1 sets what play would have set before the battle (the player in the Institute's faction and out of the Brotherhood's, BoSKickOut 20 the Brotherhood's kick-out, Inst307 10 to 35, its own 10); 0 and 2 to 8 set 1 and move the player to a point of the battle (6 and 7 set its phases' stages too). Nothing else sets 0 or 1: in play the battle starts at 10, set by Inst307 30" },
        // The kick-out chains' other quickstarts, found with -NeverSet (stages nothing the scan can see sets from play) and
        // the designers' own notes on them (the stage's log entry's note, NAM2, which -NeverSet prints).
        new Debug { Quest = "MQ302", From = 0, To = 3, Why = "The Nuclear Option's quickstarts (the designers' notes: \"TESTING - quickstart\", \"... BOS\", \"... Minutemen\", \"... RR\"): 1, 2 and 3 set the story done up to the attack on the Institute by the Brotherhood's, the Minutemen's or the Railroad's way, and 0; 2 goes through Min301 0 (the Institute the player's enemy, and its kick-out where the Minutemen are the last way on), 3 through MQ302RR 10. Nothing else sets them: the quest's start-up stage is 10" },
        new Debug { Quest = "MQ302RR", From = 10, To = 10, Why = "The Nuclear Option's quickstart on the Railroad's side (the note: \"Quick Start\"): only MQ302 3, a quickstart, sets it" },
        new Debug { Quest = "MQ206", From = 0, To = 0, Why = "The Molecular Level's quickstart (the note: \"TESTING ONLY\"): only MQ302 0 and Min301 0, quickstarts, set it, and it is no start-up stage" },
        new Debug { Quest = "Min301", From = 0, To = 1, Why = "the Minutemen's quickstarts (the notes: 0 \"quickstart\", 1 \"trigger MinDefendCastle\"): 0 sets the Minutemen's quests and the story done up to here and calls MinutemenCentralScript.CheckForMQAdvance (the Institute the player's enemy, its kick-out and X6-88 dismissed where the Minutemen are the last way on); 1 sets 0. Only quickstarts set them (0: MQ302 2 and its own 1; 1: MinDestBoS 0); play starts the quest at 10" },
        new Debug { Quest = "MinDestBoS", From = 0, To = 0, Why = "With Our Powers Combined's quickstart (the note: \"quickstart\"): sets Min301 1 and The Nuclear Option done. Nothing sets it: the quest's start-up stage is 10" },
        new Debug { Quest = "RR303", From = 1, To = 4, Why = "Precipice of War's debug stages (the notes: \"DEBUG\", \"Debug leaving the Prydwen\", \"TEST\", \"TEST 2\"): 1 sets the Railroad's quests done up to here, makes the Brotherhood the player's enemy and sets BoSKickOut 20 (its kick-out, Danse dismissed). Only RR304 1, a quickstart, sets 1; nothing sets 2 to 4" },
        new Debug { Quest = "RR304", From = 1, To = 2, Why = "Rockets' Red Glare's debug stages (the notes: \"Debug\", \"Test\"): 1 sets RR303 1 (the Brotherhood's kick-out) and the quest up to its 100. Only its quickstarts 20, 25 and 30 set 1; nothing sets 2. Play starts the quest at 100, set by RR201 1050" },
        new Debug { Quest = "RR304", From = 20, To = 20, Why = "Rockets' Red Glare's quickstart (the note: \"DEBUG: Go to Police Station\"): sets 1 and moves the player to the Cambridge police station. Nothing sets it. In play the Brotherhood is turned on the Railroad's player by RR303's own stages and BoSKickOut's aliases" },
        new Debug { Quest = "RR304", From = 25, To = 27, Why = "Rockets' Red Glare's quickstarts (the notes: \"DEBUG: Cleared Police Station\", \"DEBUG: Tink & Deacon to player\", \"DEBUG: Boarding the birg\"): 25 sets 1 and moves the player to the station's roof; 27 sets 26. Nothing else sets them" },
        new Debug { Quest = "RR304", From = 30, To = 30, Why = "Rockets' Red Glare's quickstart (the note: \"DEBUG: On Prydwen\"): sets 1 and moves the player onto the Prydwen. Nothing sets it" },
        new Debug { Quest = "MQ00", From = 10, To = 10, Why = "the story's helper quest's debug stage (the note: \"Simulate the player getting kicked out of the Brotherhood\"): calls MQ00Script.MQFactionKickoutPlayer for the Brotherhood. Nothing sets it: the kick-outs call the function themselves" },
        new Debug { Quest = "MQ00", From = 30, To = 30, Why = "the same for the Railroad (the note: \"Simulate the player getting kicked out of the Railroad\")" },
        new Debug { Quest = "MQ00", From = 40, To = 40, Why = "the same for the Institute (the note: \"Simulate the player getting kicked out of the Institute\")" },
        new Debug { Quest = "MQ00", From = 50, To = 50, Why = "the story's helper quest's debug stage (the note: \"Simulate endgame binks (Institute Destroyed)\"): plays the ending's film and sets MQ302 900. Nothing sets it: play gets there by MQ302 855" },
        new Debug { Quest = "MQ00", From = 60, To = 60, Why = "the same for the other ending (the note: \"Simulate endgame binks (Institute Saved)\"); play gets there by Inst308 30" },
        // Every other quickstart in -NeverSet's list that sets a stage a friend's game would be sent: a stage nothing the scan
        // sees sets from play, no start-up stage, and with a designer's note that says it is a debug, test or quick start
        // ("Debug", "TESTING ONLY", "QUICKSTART", "Setup stage for playtesting", "Shortcut", "[QS]"). Two of that kind are
        // left out on purpose: DN070 5 ("Quickstart - Treasures Entryway", a SAFE quest's move into its own cave), which the rig's
        // follow step (tools/rig/steps.sh step_follow) sets at the host's console as its friend's stage that moves the
        // player; and MQ105 0 ("TESTING ONLY"), whose row QuestSync's RepairStory judges its start of a stuck MQ105 by.
        new Debug { Quest = "MS07a", From = 0, To = 0, Why = "the note: \"DEBUG - Companion Interjection Quickstart\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MQ106", From = 0, To = 0, Why = "the note: \"TESTING ONLY - JUMPSTART QUEST\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MQ202", From = 0, To = 0, Why = "the note: \"TESTING ONLY\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MQ203", From = 0, To = 8, Why = "the notes: 0 \"quickstart main quest\", 1 \"TESTING - start memory A (child Kellogg)\", 2 \"TESTING - start memory B (San Francisco)\", 3 \"TESTING - start memory C (West Coast)\", 4 \"TESTING - start memory D (Midwest)\", 5 \"TESTING - start memory E (Institute first meeting)\", 6 \"TESTING - start memory F (MQ101 sequence)\", 7 \"TESTING - start memory G (Kellogg getting implants)\", 8 \"TESTING - start memory H (Kellogg with Shaun)\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "MS07c", From = 0, To = 1, Why = "the notes: 0 \"QUICKSTART\", 1 \"AUDIO DEBUG\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "MS07c", From = 190, To = 190, Why = "the note: \"DEBUG - Give player all tapes, teleport him and Nick to Andrew station\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MS16", From = 1, To = 1, Why = "the note: \"Debug Quest Start\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MS16", From = 59, To = 59, Why = "the note: \"Shortcut - Mel dig start\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MS16", From = 89, To = 89, Why = "the note: \"Shortcut - Start Subway Dig\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS200", From = 0, To = 0, Why = "the note: \"Debug Start\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS201", From = 1, To = 4, Why = "the notes: 1 \"Quest PreFlight QuickStart\", 2 \"Quest PostFlight QuickStart\", 3 \"Quest Post Briefing QuickStart\", 4 \"Quest Maxson Done QuickStart\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "BoS201", From = 35, To = 35, Why = "the note: \"Debug - Flight Avoidance Override\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoSM02", From = 0, To = 9, Why = "the notes: 0 \"Debug: Start at Kells\", 1 \"Debug Setup\", 2 \"Debug: Warp to Gavil\", 3 \"Debug: Warp Post-Gavil\", 4 \"Debug: Warp to Evidence\", 5 \"Debug: Confront Lucia\", 6 \"Debug: Shadow Clarke\", 7 \"Debug: Enter Ruins\", 8 \"Debug: Confrontation\", 9 \"Debug: Kells\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "RR101", From = 5, To = 6, Why = "the notes: 5 \"Debug Close Quest\", 6 \"Start quest debug\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "InstM01_OLD", From = 0, To = 0, Why = "the note: \"Setup stage for playtesting\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS201B", From = 0, To = 0, Why = "the note: \"Quest Debug\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS202", From = 1, To = 2, Why = "the notes: 1 \"Quest Debug Interior\", 2 \"Quest Debug Flight\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "MinVsInst", From = 0, To = 1, Why = "the notes: 0 \"quickstart - Minutemen Version\", 1 \"quickstart - Gunner version\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "DialogueRailroad", From = 1, To = 1, Why = "the note: \"Debug: Open radiants\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS100", From = 0, To = 0, Why = "the note: \"Debug Start Scene\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MQ204", From = 0, To = 0, Why = "the note: \"TESTING ONLY - QUICKSTART\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS101", From = 0, To = 2, Why = "the notes: 0 \"Debug - Setup Testing\", 1 \"Debug - ArcJet Outside\", 2 \"Debug - ArcJet Engine Core\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "RR301", From = 10, To = 10, Why = "the note: \"DEBUG: Quest available\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "InstM01", From = 0, To = 0, Why = "the note: \"Setup stage for playtesting\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "Inst305", From = 0, To = 0, Why = "the note: \"test start phase\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "InstM02", From = 0, To = 0, Why = "the note: \"Startup Stage for Playtesting\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "RRM02", From = 5, To = 5, Why = "the note: \"Debug start\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "Inst306", From = 0, To = 0, Why = "the note: \"test start stage\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoSM04", From = 0, To = 4, Why = "the notes: 0 \"Debug Quest Start; Warp to Kells\", 1 \"Debug Setup\", 2 \"Debug Warp to Virgil\", 3 \"Debug Warp to Virgil\", 4 \"Debug Warp to Kells\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "BoSM01", From = 0, To = 1, Why = "the notes: 0 \"Debug: Quest Start; In Brotherhood; Warp to Danse\", 1 \"Debug: Quest Start; In Brotherhood; Warp to Kells\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "InstM03", From = 0, To = 0, Why = "the note: \"Setup stage for playtesting\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "RR201", From = 5, To = 5, Why = "the note: \"Debug: Initial\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "RR201", From = 10, To = 10, Why = "the note: \"Debug: To Patriot\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "RR201", From = 15, To = 15, Why = "the note: \"Debug: To Z1-14 First Meet\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "RR201", From = 20, To = 20, Why = "the note: \"Debug: To Desdemona Debrief\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "RR201", From = 25, To = 25, Why = "the note: \"DEBUG: Back to Institute\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS302", From = 0, To = 2, Why = "the notes: 0 \"Quest Debug Startup\", 1 \"Quest Debug - Confront Danse at Bunker\", 2 \"Quest Debug - Confront Maxson\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "BoS302B", From = 0, To = 0, Why = "the note: \"Debug Startup\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoSM00", From = 0, To = 0, Why = "the note: \"Quickstart\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "MS05B", From = 252, To = 253, Why = "the notes: 252 \"DEBUG - Teleport to Delivery location\", 253 \"DEBUG - Teleport to Lynn Woods\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "COMCaitQuest", From = 0, To = 0, Why = "the note: \"Debug Start Quest\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "BoS304", From = 0, To = 2, Why = "the notes: 0 \"Debug Start\", 1 \"Prime Debug Startup\", 2 \"Prime Debug Startup 2 (no moveto and additional setstages)\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "BoS305", From = 0, To = 0, Why = "the note: \"Debug Start\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DN120Misc", From = 0, To = 0, Why = "the note: \"Debug Start\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03MQ04", From = 1, To = 1, Why = "the note: \"quickstart\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03MQ06", From = 1, To = 3, Why = "the notes: 1 \"Debug stage\", 2 \"Debug stage - Start wind farm path only\", 3 \"Debug stage - start nuke key path only\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "DLC03FarHarborFF03a", From = 0, To = 0, Why = "the note: \"QUICKSTART\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03FarHarborS01", From = 0, To = 0, Why = "the note: \"QUICKSTART\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03FarHarborS02", From = 0, To = 0, Why = "the note: \"QUICKSTART\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03AtomM01", From = 0, To = 0, Why = "the note: \"Debug\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03MQ06a", From = 1, To = 1, Why = "the note: \"DEBUG\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03MQ02Fight", From = 5, To = 5, Why = "the note: \"Quickstart: End Fight\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03_V118_Quest2", From = 0, To = 0, Why = "the note: \"QuickStart\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC01MQ02", From = 0, To = 0, Why = "the note: \"Debug\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC01MQ04", From = 0, To = 3, Why = "the notes: 0 \"Debug Stage\", 1 \"Got Beacon Debug Stage\", 2 \"Hagen Debug Stage\", 3 \"Build Jezebel Debug Stage\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "DLC04MQ02", From = 0, To = 0, Why = "the note: \"Debug\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC04MQ03", From = 0, To = 5, Why = "the notes: 0 \"Debug Quest Startup\", 1 \"Debug Galactic Zone Cleared\", 2 \"Debug Dry Rock Gulch Cleared\", 3 \"Debug Kiddie Kingdom Cleared\", 4 \"Debug Safari Cleared\", 5 \"Debug Bottling Plant Cleared\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "DLC04MQ04", From = 1, To = 4, Why = "the notes: 1 \"DEBUG START\", 2 \"DEBUG START - Player claimed first settlement. Send player to start second.\", 3 \"DEBUG START - Player claimed vassal. Begin Raider attack.\", 4 \"DEBUG START - Player needs to build final settlement, get settlement happiness to XX%\"; nothing the scan sees sets them from play" },
        new Debug { Quest = "DLC04MS01", From = 550, To = 550, Why = "the note: \"DEBUG: Move player & Sierra\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC04_KiddieKingdomMain", From = 6, To = 6, Why = "the note: \"DEBUG: Jump To Rooftop\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC01MQ05", From = 0, To = 0, Why = "the note: \"[QS] Ada is follower\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC04MQ01", From = 0, To = 3, Why = "the notes: 0 \"[QS] Nuka-Station\", 1 \"[QS] Cola-Cars Entrance (Harvey Alive)\", 2 \"[QS] Locker Rom (Harvey Alive)\", 3 \"[QS] Arena Post Fight\"; nothing the scan sees sets them from play" },
        // A SILENT quest's stage 0 that moves the player and is no start-up stage, which SafeScan.QuickStart leaves shared
        // only because CA_QuestStage_Bump (the companions' affinity events) stores its stage number; that sets no stage.
        // It sets no stage a friend's game would be sent either: its row is the move alone, never offered to a friend.
        new Debug { Quest = "DLC04Grandchester", From = 0, To = 0, Why = "the note: \"Quickstart\" (MoveTo DLC04_MS03_Start); nothing the scan sees sets it from play, and no other stage is set by it" },
        // The quickstarts of the quests shared since the list above was made (the add-ons' faction outcomes, the teleport quests,
        // the SILENT list's growth), found as those were: -NeverSet lists them, each with a designer's note naming it a debug
        // stage or a quickstart, or (CZ 1) a move to a debug marker.
        new Debug { Quest = "CZ", From = 1, To = 1, Why = "Combat Zone's quickstart: MoveTo CZDebugMarker, and nothing else; nothing the scan sees sets it from play" },
        new Debug { Quest = "RRAct3Pickup", From = 1, To = 2, Why = "the notes: 1 \"Debug: Supported MM\", 2 \"Debug: Supported BoS\": each sets the Institute's way in (MQ206Min 1500 or MQ206BoS 1500, the teleporter) and puts the player in the Institute's faction; nothing the scan sees sets them from play" },
        new Debug { Quest = "DLC03MQ00", From = 10, To = 10, Why = "the note: \"DEBUG - Quickstart the DLC03 Main Quest\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC03CoA_DialogueNucleusEntrance", From = 3, To = 3, Why = "the note: \"[DEBUG] Bypass entrance quest\" (the Children of Atom's entrance guards turned on the player, and the quest's stages set); only DLC03MQ06 1 to 3, quickstarts, set it" },
        new Debug { Quest = "DLC03AtomM01_CombatStop", From = 1, To = 1, Why = "the note: \"DEBUG - Set Children friendly to player\"; nothing the scan sees sets it from play" },
        new Debug { Quest = "DLC01Lair", From = 3, To = 9, Why = "the Mechanist's lair's quickstarts (the notes: 3 \"Quickstart - Facility Entrance\", 4 \"Quickstart - Production Wing Entrance\", 5 \"Quickstart - Bypass Path\", 6 to 8 \"Quickstart - Mechanist Battle, Phase 1\" to \"3\", 9 \"Quickstart - Free\"): each moves the player to a point of the lair and sets its stages up to there (8 the Mechanist's robots the player's enemy); nothing the scan sees sets them from play" },
        new Debug { Quest = "DLC04MQ05", From = 0, To = 3, Why = "Power Play's quickstarts (the notes: 0 \"Debug Start Quest - Disciples Enemy\", 1 \"... - Operators Enemy\", 2 \"... - Pack Enemy\", 3 \"... - Disciples Enemy Post Talk\"): each moves the player to the start and sets 8 (a gang the player's enemy); nothing the scan sees sets them from play" },
    };
    public static bool IsDebug(string a_edid, int a_stage) { return DEBUG.Any(d => Is(d.Quest, a_edid) && a_stage >= d.From && a_stage <= d.To); }

    // Every stage with a kDebug row, SafeScan loaded: DEBUG's, and a SAFE or SILENT quest's stage 0 that SafeScan.QuickStart
    // proves a developer's quickstart (MS07's, BoSM04Postquest's, DN154's, MS05BTapes'; Run(..., true) gives it its row).
    public static bool IsQuickstart(int a_file, GearScan.Quest a_quest, int a_stage)
    {
        if (IsDebug(a_quest.Edid, a_stage)) return true;
        if (a_stage != 0) return false;
        var shared = QuestScan.Shared(a_quest, a_file);
        return (shared == "shared (safe)" || shared == "shared (silent)") && SafeScan.IsQuickStart(a_quest, a_file);
    }

    public static double Waits(GearScan.Func f)
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
                    // a SAFE or SILENT quest's developer's quickstart stage 0 (SafeScan.QuickStarts): kDebug, never set from
                    // a friend's game, and not run as the quest starts
                    if (stage == 0 && a_safeOnly && SafeScan.IsQuickStart(quest, file))
                    {
                        var how = SafeScan.QuickStarts[Tuple.Create(file, quest.Id & 0xFFFFFF)];
                        sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, 0, kDebug, 0 }},  // {2}: a developer's quickstart ({3})", file, quest.Id & 0xFFFFFF, quest.Edid, how.Substring(0, how.IndexOf(" (", StringComparison.Ordinal))));
                        first = false;
                        continue;
                    }
                    var moves = MoveScan.StageMoves(file, quest, stage);
                    // a stage that makes the player a faction's enemy is no row here: HostileScan's (StageRules.h HOSTILE)
                    var takes = stages[stage].SelectMany(f => QuestScan.Takes(f)).Where(t => t.Item2.Length > 0 && !t.Item2.StartsWith("temp", StringComparison.OrdinalIgnoreCase)).ToList();
                    var wait = (int)Math.Min(255, Math.Round(10 * stages[stage].Sum(f => Waits(f))));
                    bool debug = IsDebug(quest.Edid, stage);
                    if (moves == null && takes.Count == 0 && wait == 0 && !debug) continue;
                    if (stage == 0 && !quest.Startup.Contains(0) && !debug)
                    {
                        left.Add(string.Format("#   {0} {1:X6} 0 {2}: {3}", file, quest.Id & 0xFFFFFF, quest.Edid, moves ?? "waits " + wait));
                        continue;
                    }
                    var flags = new List<string>();
                    if (moves != null) flags.Add("kMoves");
                    if (takes.Count > 0) flags.Add("kPays");
                    if (debug) flags.Add("kDebug");
                    var what = new List<string>();
                    if (moves != null)
                    {
                        var hops = moves.Split(new[] { " -> " }, StringSplitOptions.None);
                        var effect = hops[hops.Length - 1];
                        effect = effect.Substring(effect.LastIndexOf(": ", StringComparison.Ordinal) + 2);
                        what.Add(hops.Length > 1 ? effect + " (by " + hops[0] + ")" : effect);
                    }
                    if (takes.Count > 0) what.Add(string.Join(" ", takes.Select(t => t.Item1 + "(" + t.Item2 + t.Item3 + ")").Distinct().ToArray()));
                    var comment = (first ? quest.Edid + (what.Count > 0 ? ": " : "") : "") + string.Join("; ", what.ToArray()) + (debug ? " [a quickstart]" : "");
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

// The rows of src/game/StageRules.h's QUICKSTART_SETS: for each developer's quickstart (a kDebug row: RuleScan's DEBUG, and
// the SAFE and SILENT quests' stage 0s SafeScan.QuickStart proves, RuleScan.IsQuickstart) every stage that setting it at the console sets, as deep as it goes (MoveScan.Sets: its fragment, the script functions
// it calls, the stages it sets or starts of any quest, shared or not, and what those set in turn; the quest's own
// start-up stages too, since a console's setstage starts it). One row per stage set and quickstart (its quest and its
// stage, any stage number), sorted by the stage set. Only the stages of quests a friend's stage is set on have a row (shared by type, a finale, a dialogue
// quest, QuestFilter.h's SAFE and SILENT; the others are walked through but never sent anyway), and a stage that is a
// quickstart itself has none (never sent as it is). A stage the quest's record doesn't have is left out (SetStage does
// nothing). Listed after the rows: what each quickstart sets, by count, and each SetStage or Start on the way that
// could not be followed (a scene's Start is no quest's; a stage that is no literal is not known).
public static class QuickstartScan
{
    public static string Run(string data)
    {
        MoveScan.Load(data);
        var rows = new SortedDictionary<Tuple<int, uint, int, int, uint, int>, string>();
        var unresolved = new SortedSet<string>();
        var counts = new List<string>();
        int notShared = 0, quickstarts = 0, missing = 0;
        for (int file = 0; file < MoveScan.perFile.Count; file++)
        {
            foreach (var quest in MoveScan.perFile[file].Values.OrderBy(q => q.Id & 0xFFFFFF))
            {
                foreach (var stage in quest.StageIds.Distinct().OrderBy(s => s))
                {
                    if (!RuleScan.IsQuickstart(file, quest, stage)) continue;
                    var open = new List<string>();
                    int n = 0;
                    foreach (var set in MoveScan.Sets(file, quest, stage, true, open))
                    {
                        if (RuleScan.IsQuickstart(set.File, set.Quest, set.Stage)) { quickstarts++; continue; }
                        if (!set.Quest.StageIds.Contains(set.Stage)) { missing++; continue; }
                        if (QuestScan.Shared(set.Quest, set.File) == "not shared" || !QuestScan.ReachesFriend(set.Quest, set.File)) { notShared++; continue; }
                        var key = Tuple.Create(set.File, set.Quest.Id & 0xFFFFFF, set.Stage, file, quest.Id & 0xFFFFFF, stage);
                        rows[key] = set.Quest.Edid + " " + set.Stage + " <- " + quest.Edid + " " + stage;
                        n++;
                    }
                    counts.Add(string.Format("#   {0} {1}: {2}", quest.Edid, stage, n));
                    foreach (var o in open) unresolved.Add(o);
                }
            }
        }
        var sb = new StringBuilder();
        foreach (var r in rows)
            sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, 0x{4:X6}, {5} }},  // {6}", r.Key.Item1, r.Key.Item2, r.Key.Item3, r.Key.Item4, r.Key.Item5, r.Key.Item6, r.Value));
        sb.AppendLine(string.Format("# {0} rows; left out (counted once per quickstart): {1} of quests no friend's stage is set on, {2} that are quickstarts themselves, {3} the quest's record doesn't have", rows.Count, notShared, quickstarts, missing));
        sb.AppendLine("# stages set, by quickstart:");
        foreach (var c in counts) sb.AppendLine(c);
        if (unresolved.Count > 0)
        {
            sb.AppendLine("# not followed (the stage whose script it is: the function: the call):");
            foreach (var u in unresolved) sb.AppendLine("#   " + u);
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
    // Or the script picks the faction as it runs, and the walk names only the first it meets: DLC04MQ05 8 (Power Play) sets
    // the snubbed gang on the player through DLC04GangSetup, which reads the gang from the DLC04EnemyGang global the stage
    // carries (StageRules.h CARRIES): one of the three gangs' factions (the Disciples, the Operators, the Pack). Such an
    // entry names the factions by editor ID (Factions), in place of what the walk found: one row each. Which of them the
    // hold waits for is the game's to say, by the value that came with the stage (StageRules.h HOSTILE_WHEN, by hand:
    // DLC04EnemyGang 1, 2, 3 in that order), any of the three only when none came. Its quickstarts 1, 2 and 3 (a rank set, a MoveTo to the quest's
    // test mark, then its 5 and 8; kMoves rows, never set in a game played through) name none and get no row: a mover's
    // row is the call QuestSync makes without the stage, and which gang it would be is not known here.
    class ByHand { public string Quest; public int Stage; public string PropertyQuest; public string Property; public int Index; public string[] Factions; }
    static readonly ByHand[] BY_HAND = {
        new ByHand { Quest = "MQ00", Stage = 10, PropertyQuest = "MQ00", Property = "Factions", Index = 0 },
        new ByHand { Quest = "MQ00", Stage = 30, PropertyQuest = "MQ00", Property = "Factions", Index = 2 },
        new ByHand { Quest = "MQ00", Stage = 40, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "MQ207", Stage = 5, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "MQ207", Stage = 200, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "MQ302", Stage = 400, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "InstKickOut", Stage = 100, PropertyQuest = "MQ00", Property = "Factions", Index = 3 },
        new ByHand { Quest = "DLC04GZMainQuest", Stage = 10, PropertyQuest = "DLC04GZMainQuest", Property = "GalacticZoneEnemyFactions", Index = -1 },
        new ByHand { Quest = "DLC04MQ05", Stage = 1, Factions = new string[0] },
        new ByHand { Quest = "DLC04MQ05", Stage = 2, Factions = new string[0] },
        new ByHand { Quest = "DLC04MQ05", Stage = 3, Factions = new string[0] },
        new ByHand { Quest = "DLC04MQ05", Stage = 8, Factions = new[] { "DLC04GangDisciplesFaction", "DLC04GangOperatorsFaction", "DLC04GangPackFaction" } },
    };

    // The factions a BY_HAND entry names by editor ID for this quest's stage, in place of the walk's (none: no row), or null.
    static List<KeyValuePair<string, uint>> Named(GearScan.Quest a_quest, int a_stage, Dictionary<string, uint> a_factions)
    {
        var entry = BY_HAND.FirstOrDefault(h => h.Factions != null && Is(h.Quest, a_quest.Edid) && h.Stage == a_stage);
        if (entry == null) return null;
        var result = new List<KeyValuePair<string, uint>>();
        foreach (var name in entry.Factions)
        {
            uint id;
            if (!a_factions.TryGetValue(name, out id)) throw new InvalidOperationException("BY_HAND names no faction of the game's: " + name);
            result.Add(new KeyValuePair<string, uint>(name, id));
        }
        return result;
    }

    // Script functions on a dismissal's chain that go on only on a condition of the game they run in, read by hand: a
    // stage whose chain to the dismissal passes one dismisses the companion in some games and not in others, so its
    // DISMISSES row is marked (the last column; "[only if ...]" in the comment) and gives no HUD warning. The walk
    // follows every call and stage a fragment or function names, whatever branch it is in, so a function that sets a
    // kick-out behind a test has to be named here.
    static readonly Dictionary<string, string> CONDITIONAL = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "MinutemenCentralScript.CheckForMQAdvance", "sets MinDefendCastle 15 (which starts the Institute's kick-out) only when the Minutemen are the story's last way on in the game it runs in: the main quest not done, the player thrown out of the Brotherhood and the Railroad or their quests failed, not yet the Institute's enemy, the Castle held with enough settlements and Old Guns or Taking Independence done. Every stage that moves the Minutemen on calls it (SetProgress, a recruitment done, MQ302's end), and nearly always it does nothing" },
    };
    // The CONDITIONAL function a chain passes, or null.
    static string Conditional(string[] a_hops)
    {
        foreach (var hop in a_hops) foreach (var name in CONDITIONAL.Keys) if (Is(hop, name)) return name;
        return null;
    }

    // The factions a BY_HAND entry names for this quest's stage, or null.
    static List<uint> Hand(GearScan.Quest a_quest, int a_stage)
    {
        var entry = BY_HAND.FirstOrDefault(h => h.Factions == null && Is(h.Quest, a_quest.Edid) && h.Stage == a_stage);
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
        var quickstarts = new List<string>();
        var unresolved = new List<string>();
        var counts = new SortedDictionary<string, int>();
        int rows = 0, byEdid = 0;
        var dismisses = new StringBuilder();  // StageRules.h DISMISSES: the HOSTILE stages that turn the faction's companion on the player too
        int dismissals = 0, conditionalDismissals = 0;
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
                        {
                            var condition = Conditional(dhops);
                            if (condition != null) conditionalDismissals++;
                            dismisses.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, 0x{4:X6}, {5}, {11} }},  // {6}{7}{8}: {9}'s {10}{12}", file, quest.Id & 0xFFFFFF, stage, who.Item2, who.Item1.Id & 0xFFFFFF, who.Item3,
                                firstDismissal ? quest.Edid + ": " : "", deffect, dhops.Length > 1 ? " (by " + dhops[0] + ")" : "", who.Item1.Edid, alias.Name, condition != null ? "true" : "false",
                                condition != null ? " [only if " + condition + " goes on]" : ""));
                        }
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
                    // by hand, in place of the walk's first: the factions the script picks from as it runs, or none (no row)
                    var named = Named(quest, stage, factions);
                    if (named != null && named.Count == 0)
                    {
                        quickstarts.Add(string.Format("#   {0} {1:X6} {2} {3}: {4}", file, quest.Id & 0xFFFFFF, stage, quest.Edid, what));
                        continue;
                    }
                    if (named != null)
                    {
                        var by = effect.StartsWith("SetPlayerEnemy", StringComparison.OrdinalIgnoreCase) ? "kSetPlayerEnemy"
                            : effect.StartsWith("AddToFaction", StringComparison.OrdinalIgnoreCase) ? "kAddToFaction" : "kSetEnemy";
                        var walked = SafeScan.HostileFaction(chain);
                        foreach (var kv in named)
                        {
                            sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, 0x{4:X6}, {8} }},  // {5}{6}{7}", file, quest.Id & 0xFFFFFF, stage, kv.Value >> 24, kv.Value & 0xFFFFFF,
                                first ? quest.Edid + ": " : "", walked == null ? what : what.Replace(walked, kv.Key), " [by hand: one of the " + named.Count + " the script picks from]", by));
                            first = false;
                            rows++;
                        }
                        int m; counts.TryGetValue(shared, out m); counts[shared] = m + 1;
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
        if (quickstarts.Count > 0)
        {
            sb.AppendLine("# left out by hand (BY_HAND names no faction for it), a quickstart whose faction the script picks as it runs:");
            foreach (var l in quickstarts) sb.AppendLine(l);
        }
        if (unresolved.Count > 0)
        {
            sb.AppendLine("# not resolved to a faction (no row; add by hand):");
            foreach (var u in unresolved) sb.AppendLine(u);
        }
        sb.AppendLine("# " + rows + " rows (" + string.Join(", ", counts.Select(kv => kv.Value + " " + kv.Key).ToArray()) + "); " + byEdid + " by editor ID; " + unresolved.Count + " unresolved");
        sb.AppendLine("# DISMISSES: the HOSTILE stages that turn the faction's own companion on the player too (SafeScan.StageDismisses)");
        sb.Append(dismisses.ToString());
        sb.AppendLine("# " + dismissals + " rows (" + conditionalDismissals + " only on a condition: CONDITIONAL)");
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
// (MQ101, DogmeatQuest) the rule leaves out by itself, for a stage, and stays there in case it
// changes. The quests the rule passes but for their objectives (a location's quest, DN035) are SILENT's, printed after
// SAFE, less those denied by name or by hand or that stop early (WhySilent), each listed with why. Then,
// for each quest left out for a stage, the first stage found and its chain. Last, the payments reached through a shared
// stage, SAFE's, SILENT's or any other quest's a friend's stage is set on (Payments).
public static class SafeScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }

    static readonly System.Text.RegularExpressions.Regex COMPANION = new System.Text.RegularExpressions.Regex("^(COM|Followers|CA_|CIS_|.*Affinity|.*Companion)", System.Text.RegularExpressions.RegexOptions.IgnoreCase);  // ComX688 is X6-88's own, as COMNick is Nick's

    // FollowersScript.AllowCompanion(actor, MakeCompanionIfNoneCurrently, ForceCompanion) with both flags False: the actor
    // is only taken out of DisallowedCompanionFaction (with either True, SetCompanion or SetDogmeatCompanion). In a friend's
    // game that touches only their own copy of the NPC, who is nobody's companion there (DisallowCompanion is the same:
    // its DismissCompanion returns unless the actor is the current companion). Not a companion change.
    public static bool AllowsOnly(GearScan.Instr i)
    {
        return i.Op == 23 && Is(i.Args[0], "AllowCompanion") && i.Args.Count >= 5 && Is(i.Args[3], "False") && Is(i.Args[4], "False");
    }

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
    // The stages src/game/StageRules.h's RULES flag kPays or kEach: never run in a friend's game (a payment is skipped whole;
    // a companion's owner's stage is marked done there without its script), so what their fragments do doesn't make
    // their quest each player's (FFDiamondCity08's Nuka-Cola for Sheffield, MQ202NickPostSurgery's recruit of Nick).
    static readonly HashSet<Tuple<int, uint, int>> skipped = new HashSet<Tuple<int, uint, int>>();
    // Stages left unjudged by hand, with why: what they touch is every game's already.
    static readonly Dictionary<Tuple<int, uint, int>, string> SKIPPED_BY_HAND = new Dictionary<Tuple<int, uint, int>, string> {
        { Tuple.Create(0, 0x0FAC86u, 100), "MQ202NickPostSurgery 100: Nick's affinity conditions (Valentine_AffinityCondition_HatredAllowed, Valentine_MurderToggle), which the story's MQ204 200 sets in every game through its 95 anyway; its AllowCompanion has neither flag" } };
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
            if (AllowsOnly(i)) { players.Remove(i.Args[2]); continue; }
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
        if ((q.Flags & 0x101) == 0 && !RunsOnce(q, a_file)) return "repeatable";
        var aliases = new Dictionary<int, GearScan.Alias>();
        foreach (var a in q.Aliases) if (!aliases.ContainsKey(a.Id)) aliases[a.Id] = a;
        if (!ALLOW_FILLS.ContainsKey(q.Edid) && q.Aliases.Any(a => PerGame(aliases, a.Id, 0))) return "an alias each game fills";
        return null;
    }

    // What each stage does, followed into the scripts it calls and the stages it sets or starts as -Moves follows them,
    // but for the stages of a quest every session with the story shares by its type (the story, a side or faction quest,
    // a dialogue quest, a finale: QuestScan.Shared) and is no radiant or PER_GAME quest, whose stages no friend's game is
    // sent (QuestScan.ReachesFriend): what one of those does a friend's game is sent anyway, SAFE or not.
    static bool Skipped(int a_file, GearScan.Quest q, int a_stage) { return skipped.Contains(Tuple.Create(a_file, q.Id & 0xFFFFFF, a_stage)); }
    static readonly MoveScan.Walk walk = new MoveScan.Walk { Direct = Direct, Target = Target, Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Allows = AllowedStart, Skip = Skipped, Unwalked = Radiant };

    // The starts of a quest each game runs its own way that are let through, by "<the quest whose stage starts it> <the quest
    // started>", with why: what the started quest does is each game's own and harmless twice (each game running it for its
    // own player is what a played-through game does), or every game runs it from a shared story stage's own fragment
    // anyway. Its stages are walked as any other quest's (the rule holds for them),
    // but a radiant's (RadiantRules.h, shared by its run: Radiant): its stages reach a friend's game as that run's, and
    // a kick-out sets only a running one's failed stage (the walk doesn't see the IsRunning branch, and would read the
    // radiant's start-up as its own start of what it starts).
    static readonly Dictionary<string, string> ALLOW_STARTS = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "DN054 DN054Radio", "Atom Cats Garage's 1 starts the Atom Cats' radio station (repeatable, no objectives) and sets RadioDCTrackReady: each game plays its own station, as every radio station is each game's (DENY_PATTERNS)" },
        { "DLC04RaiderKickout DLC04PackFights", "Open Season's 20 sets DLC04PackFights 75, the Pack's arena fights at Bradberton (repeatable, no objectives; the story manager starts one in each game as its player enters, CLOC): 75 puts the arena's animals in the Pack's faction, so they turn on the player with the gang. A run it starts in a friend's game, if none runs there, is the one that game starts by itself on entering" },
        { "DLC04SafariAdventureQuest DLC04SafariAdventurePOST", "Safari Adventure's 2000 (its end, every path's) sets DLC04SafariAdventurePOST 0, Cito's after-quest (repeatable, no objectives, no fragment at 0): its 5 (Cito turned on the player) and 10 (the cloned gazelles) are set by each game's own dialogue and global, so each game runs its own, as a played-through game does" },
        { "DLC04MQ05 DLC04RaidSubdue", "DLC04GangSetup (Power Play's 8) sets a raid's 850 only if the raid is running (IsRunning) against the gang made the enemy: it ends the raid, each game's own as PER_GAME; a raid not running is never started by it" },
        { "DLC04MQ05 DLC04RaidChaseOff", "the same" },
        { "DLC04MQ05 DLC04RaidWipeOut", "the same" },
        { "DLC04MQ05 DLC04RaidCoerce", "the same" },
        { "DLC04GZTrackingQuest DLC04GZNukaGalaxy", "DLC04GZTrackingQuest 2000 (set by ParkPowerOn, Power Play's 6 and 120: Nuka-World's power on) sets the Nuka-Galaxy ride's 2000: its lighting and power markers on, the ride's pre-power scenes stopped; the parks' power is each game's own (DLC04PowerTracker is DENIED), and a friend's 120 runs ParkPowerOn there as the player's own would" },
        { "DLC04MQ05 DLC04_NUK001_CrowdManager", "DLC04GangSetup sets Nuka-Town's crowd manager's 11 (the crowd without the enemy gang) only if it is running (IsRunning): the crowd is each game's own, as ambient scenes are; not running, it is never started by it" },
        { "DLC04DialogueHarvey DLC04REObjectCT03", "Harvey's 1000 (he leaves Nuka-Town for good: disabled, the quest stopped) sets DLC04REObjectCT03 1000, the end of his roadside encounter (repeatable, no objectives; the story manager starts it in each game as its player passes, its trigger an event alias): 1000 sends Harvey back to the market's marker unless its 30 is done, and rearms the encounter's own trigger. Run where no encounter runs, its aliases are empty and it does nothing, as a played-through game whose player never met him there" },
        { "DN084 DN084_Overrides", "Mass Fusion's 10 starts DN084_Overrides (repeatable, no objectives; its one stage has no fragment): the quest companion's, the player's companion's and Dogmeat's packages for the descent and the control room, each game's own followers in each game, as a played-through game does" },
        { "BoSKickOut BoSMarshal", "KickOut sets BoSMarshal 255 if BoS304 runs (the Brotherhood's named people marshalled to the airport for the ending's battle, repeatable, no objectives): 255 reverts them to their real selves, a clean-up that runs only where the battle is on, as a played-through game does" },
        { "Inst307Fight BoSMarshal", "10 sets BoSMarshal 255 (the same clean-up, as the airport battle begins)" },
        { "BoSKickOut RECheckpoint*", "KickOut sets each running RECheckpoint* (the checkpoints' attackers after a faction's win, random encounters each game runs itself) to 600, which hides its objective and ends it, when this game's player won with the Brotherhood (RECheckpointMQWonBoS): a clean-up of each game's own encounters" },
        { "RRKickOut RECheckpoint*", "100 sets each running RECheckpoint* to 600 when this game's player won with the Railroad (RECheckpointMQWonRR): the same clean-up" },
        { "BoSKickOut BoSR*", "KickOut sets each running Brotherhood radiant (BoSR01-BoSR05: RadiantRules.h's, shared by their run) to its failed stage: a clean-up of a run this game has, as a played-through game does" },
        { "RRKickOut RRR05", "300 sets RRR05 9000 (a Railroad radiant, RadiantRules.h's, shared by its run): its failed stage, the same clean-up" },
        { "Inst307Fight Inst307FightCompanionOverrides", "10 sets Inst307FightCompanionOverrides 10 (repeatable, no objectives, named as a companion's but the airport battle's own: the player's companion's packages for the battle, each game's own follower in each game, as DN084_Overrides)" },
        { "BoSKickOut VFT", "KickOut sets VFT 355 (the vertibird signal grenades' travel, repeatable): the player's vertibird calls end with their expulsion (BoSFastTravelCanUse 0 too), each game's own rides, the kick-out's outcome in both" },
        { "DN136_Attack BoSMarshal", "the Institute attack's 2000 sets the repeatable BoSMarshal 240 (the Brotherhood's squads sent on): every game sets it from the story's own MQ302 800, whose fragment sets DN136_Attack 2000" },
        { "DNPrime_BoS304 DN035_EasyCityRadioOverrides", "Liberty Prime's march's 15 starts the repeatable DN035_EasyCityRadioOverrides (Easy City Downs' radio chatter while Prime passes): every game starts it from the story's own BoS304 55, whose fragment sets DNPrime_BoS304 15, and a radio is each game's (DENY_PATTERNS)" },
        { "DNPrime_BoS304 BoSMarshal", "its 15 sets the repeatable BoSMarshal 40 too, from the same BoS304 55 in every game" },
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

    // The quests flagged neither run-once nor start-enabled that the game runs once all the same, read from
    // src/game/QuestFilter.h's RUNS_ONCE (each row says what starts it and why it never runs again): the rule takes them
    // as run-once quests (Why, EachGames), and so does QuestActors' gate.
    static HashSet<Tuple<int, uint>> runsOnce = new HashSet<Tuple<int, uint>>();

    static bool RunsOnce(GearScan.Quest q, int a_file) { return runsOnce.Contains(Tuple.Create(a_file, q.Id & 0xFFFFFF)); }
    // The same walk for an ALLOW_OUTCOMES quest: what its stages do but turn a faction hostile or friendly or give or take
    // a settlement (a companion, affinity, the inventory emptied, a story event or a quest each game runs its own way still
    // leave it out). It does not go into the workshop scripts a settlement given or taken (SetOwnedByPlayer) or a settler
    // added or removed (SETTLERS) runs: that is each game's workshop keeping its own settlement, as it does for any the
    // player owns there (the radio beacon's story event deep in WorkshopParentScript.UpdateRadioObject, which unassigning
    // a settler reaches only for a radio beacon assigned to them). Nor is someone made available as a companion (AVAILABLE)
    // a companion's change there: FollowersScript.SetAvailableToBeCompanion puts them in HasBeenCompanionFaction and shows
    // the "available as a companion" message, nothing more (no recruit, no dismissal, no affinity), the outcome every
    // played-through game has, and the first player to recruit them keeps them (src/game/Companions).
    static readonly HashSet<string> SETTLERS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "AddActorToWorkshop", "AddActorToWorkshopPUBLIC", "AddPermanentActorToWorkshopPUBLIC", "RemoveActorFromWorkshopPUBLIC" };
    static readonly HashSet<string> AVAILABLE = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "SetAvailableToBeCompanion" };
    static bool MadeAvailable(GearScan.Instr i) { return (i.Op == 23 || i.Op == 25) && AVAILABLE.Contains(i.Op == 25 ? i.Args[1] : i.Args[0]); }
    static readonly MoveScan.Walk rest = new MoveScan.Walk { Direct = c => Direct(c.Where(i => !MadeAvailable(i)).ToList(), false), Target = Target,
        Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Enter = name => !SETTLEMENTS.Contains(name) && !SETTLERS.Contains(name) && !AVAILABLE.Contains(name),
        Allows = AllowedStart, Skip = Skipped, Unwalked = Radiant };
    // The same walk for an ALLOW_KICKOUTS quest: a companion's affinity and a dismissal are let through as well (Direct's
    // a_companions), and a stage of the companion's quest named with it (AllowedKickout: Open Season's to DLC04COMGage,
    // the Brotherhood's kick-out to COMDanse, not any companion's); the rest of the rule holds.
    static readonly MoveScan.Walk kickout = new MoveScan.Walk {
        Direct = c => Direct(c.Where(i => !MadeAvailable(i)).ToList(), false, true), Target = Target,
        Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Enter = name => !SETTLEMENTS.Contains(name) && !SETTLERS.Contains(name) && !AVAILABLE.Contains(name),
        Allows = (from, to, call) => AllowedStart(from, to, call) || AllowedKickout(from, to), Skip = Skipped, Unwalked = Radiant };

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
    // settlement given or taken, a settler added or removed, or someone made available as a companion (the first found).
    static readonly MoveScan.Walk outcome = new MoveScan.Walk {
        Direct = c => {
            var a = c.FirstOrDefault(MadeAvailable);
            if (a != null) return "companion available " + (a.Op == 25 ? a.Args[1] : a.Args[0]);
            var d = Direct(c, true);
            if (d != null && (d.StartsWith("factions ") || d.StartsWith("settlement "))) return d;
            var s = c.FirstOrDefault(i => i.Op == 23 && SETTLERS.Contains(i.Args[0]));
            return s == null ? null : "settlement " + s.Args[0];
        },
        Follow = (q, file) => !QuestScan.ReachesFriend(q, file), Enter = name => !SETTLEMENTS.Contains(name) && !SETTLERS.Contains(name) && !AVAILABLE.Contains(name) };

    // The quests with an alias filled by its conditions (an alias each game fills, as Why and EachGames read them) whose
    // fill is the same in every game all the same, by editor ID with why: the conditions read a global that comes with
    // the stage that starts the quest (src/game/StageRules.h CARRIES), so every game fills it as the chooser's did. Why
    // and EachGames take them as fixed, and a Start of one is walked as any (Target).
    static readonly Dictionary<string, string> ALLOW_FILLS = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "DLC04MQ05Cleanup", "its EnemyOutposts alias (a collection by conditions) holds the enemy gang's raider outposts, by DLC04EnemyGang, which DLC04MQ05 255, the stage that starts it, carries (StageRules::CARRIES): the same outposts in every game" },
    };

    // The quests whose stages' faction or settlement outcomes are shared with the story, by the maintainer's choice
    // (2026-11-06): with sQuests = safe one player's outcome is both players', as the shared story's kick-outs already
    // are; with it off each keeps their own. By editor ID, with why, each read in its stage fragments (the stages each
    // flagged one sets too). Why lets a stage that does only that through (rest); the rest of the rule holds for them,
    // and a stage that turns a faction on the player is HOSTILE's (-Hostile; StageRules.h): it waits until none of that
    // faction is near, and is never skipped. By the maintainer's choice of 2026-11-10 (the chooser decides for both, a
    // faction's kick-out is shared): Far Harbor's endings and kick-out, objective-less (SILENT's), Nuka-World's Open Season
    // and Safari Adventure (its DLC04SafariAdventurePOST let through: ALLOW_STARTS), and DLC04MQ05 (Power Play) with
    // DLC04MQ05Cleanup (Cleaning House; the park's power and clean-up let through: ALLOW_STARTS, ALLOW_FILLS): the enemy
    // gang is the global DLC04EnemyGang, which its 5 (DLC04MakeEnemy) works out from the DLC04Rank* globals, each game's
    // own (its own flag raises and raids, DLC04RaiderControllerQuest's), so the stage carries the chooser's values of them
    // (src/game/StageRules.h CARRIES) and every game turns the same gang on its player. The add-ons' other faction
    // outcomes, Combat Zone's and the end of The Big Dig's (2026-11-11) are in too, SAFE's and SILENT's:
    // DLC04DialogueHubologists, whose other stages are each player's own (StageRules.h kOwn rows: a friend's game never
    // sets them), DLC04DialogueHarvey, whose 1000 starts the radiant DLC04REObjectCT03 (ALLOW_STARTS), CZ and
    // FFGoodneighbor07, who make Cait and Hancock available as companions (AVAILABLE), and the objective-less DLC03MQ00,
    // DLC03CoA_DialogueNucleusEntrance, DLC01MasterQuest, DLC01Lair (and DLC01Lair_ChangeLocation, which starts it) and
    // DLC04MS01SierraAttacks. With them, by the maintainer's choice of 2026-11-11 (a faction's kick-out is the chooser's
    // for both), the base game's objective-less kick-outs and faction outcomes (SILENT's): BoSKickOut, BoSKickOutSoft and
    // RRKickOut, DN084 (Mass Fusion, whose quickstarts set a kick-out, and its two final battles),
    // BoSDansePostGame, Inst307Fight, MS07cEddiePostQuestLines, DN028 and DN117 (a settlement given). A kick-out that
    // turns the faction's own companion on the player (Danse, Deacon, Gage) is ALLOW_KICKOUTS'.
    static readonly Dictionary<string, string> ALLOW_OUTCOMES = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "BoSKickOut", "the Brotherhood's kick-out (20, and 30 for Mass Fusion's: BoSKickOutScript.KickOut): the player out of the faction and its enemy, its quests failed, the airport's workshop taken, Danse too (ALLOW_KICKOUTS); set by InstMassFusion 30 and Inst307Fight 1, and by Mass Fusion's quickstarts (DN084 2, 4, 6, 8: RuleScan's DEBUG, never shared)" },
        { "BoSKickOutSoft", "the Brotherhood's soft expulsion (10: BoSKickOutSoftScript.SoftKickOut, for a player who sided against them without a fight): their quests failed or set past, BoSPostQuest 10; no faction turned, no companion dismissed (read in its script: up to 0.16.1 denied by hand as \"their companion's too\")" },
        { "RRKickOut", "the Railroad's kick-out (100): the player out of the faction and its enemy, its people made mortal, Deacon too (ALLOW_KICKOUTS), Curie's quest moved past the Railroad (COMCurieQuest 310 if it runs); 75 Glory out of her factions; 300 the Railroad's quests failed" },
        { "DN084", "Mass Fusion: its quickstarts 1-9 (RuleScan's DEBUG, never shared) set a kick-out, 2, 4, 6 and 8 the Brotherhood's (BoSKickOut 30, or 20 through InstMassFusion 30), the others the Institute's through BoS303 30; 10 starts DN084_Overrides (ALLOW_STARTS, the followers' descent packages); the rest is the building's own fight and its agitator" },
        { "DN084_BoS303Battle", "Mass Fusion's final battle on the Brotherhood's side: 200 sets DN084 450" },
        { "DN084_Inst303Battle", "Mass Fusion's final battle on the Institute's side: 200 sets DN084 450" },
        { "BoSDansePostGame", "Danse after Blind Betrayal (set by BoS302 255): 10 clears him from the kick-out's alias, 20 takes him out of the Brotherhood, puts him in their enemies' faction and makes them hostile to the player while he follows them (DansePostQuestBoSHostilityScript, each game's own companion)" },
        { "Inst307Fight", "the Institute's airport battle (Airship Down): 0 sets its 1 and moves the player to the airport (kMoves; no start-up stage), 1 puts them in the Institute's faction, out of the Brotherhood's and sets BoSKickOut 20 (the Brotherhood's kick-out) and Inst307's stages, 2-8 are its quickstarts (RuleScan's DEBUG, never shared); the rest is the battle's waves" },
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
        { "DLC03AcadiaDefense", "Acadia turned on the player (its start-up 100, set by DLC03Acadia_BoS 200, DLC03Acadia_Institute 200 or DLC03MQ05 850: SetEnemy on the player's faction, Acadia's people made mortal, its quests failed, DiMA gone for the endings); 200 Acadia destroyed (the global the endings read, DLC03MQ05 900)" },
        { "DLC03AtomM01_CombatStop", "the Nucleus's fight stopped once the player joins the Children of Atom: 1 makes them the player's allies (SetAlly), 100 ends the fight; DLC03AtomM01 100/150/750 (shared by its type) set its 10/100/750 anyway" },
        { "DLC03CoA_Kickout", "the Children of Atom's kick-out, 700 (set by DLC03CoA_DialogueNucleus 70, DLC03AtomM01 700, DLC03MQ06a 755/760, DLC03MQ06_NukeHandling 100): the player out of their faction and their enemy, the kick-out message, their quests' stages; as the story's kick-outs" },
        { "DLC03MQ06_NukeHandling", "the Nucleus nuked (Far Harbor's ending): 100 launches (a save, the launch timers, fast travel off until 500) and sets DLC03CoA_Kickout 700; 140 seals the blast doors and 150 is the blast (kills a player still inside, as in the game; a friend's 140 and 150 wait while their player is inside, StageRules.h TRAPS); 5-20 the launch key's leads; 15 gives the key's holotape (each player gets it)" },
        { "DLC03MQ06_WindFarmHandling", "Far Harbor destroyed from the wind farm (its ending): 5 digs up the code (moves the player: kMoves), 35 pays Avery's extortion (each player gets it), 50 stops the turbines and sets the fog creatures on the town (SetEnemy, not on the player), its map marker and the story's stages" },
        { "DLC04_KiddieKingdomMain", "Kiddie Kingdom: 740 starts the fight with Oswald (his faction on the player); 600 and 700 run the quest script's one-time Init, which sets the hostile ghouls on the player as they are anyway" },
        { "DLC04DialogueSettlement", "Nuka-Town's traders: 700 makes the traders and visitors the hub raiders' enemies and the player's allies (the raiders' end); 110's bloodworm meat is kPays" },
        { "DLC04RaiderKickout", "Open Season, the raiders' kick-out: 20 (DLC04SendKickOut) turns the three gangs and Shank's crime faction on the player, unlocks the power plant (DLC04PowerTracker 100), makes Nuka-Town's traders the player's allies (DLC04DialogueSettlement 700) and takes the raiders' settlements from the player (ClearRaiderSettlementOwnership); 30-60 one boss each (their deaths are every game's), 100 the four dead; Gage's turn on the player is ALLOW_KICKOUTS'" },
        { "DLC04SafariAdventureQuest", "Safari Adventure: 195 sets Cito on the player (SetEnemy with both sides neutral, StartCombat), 175/176 give the custom item (DLC04_CustomItemQuest 40), 60 the holotape; 2000 starts Cito's after-quest (ALLOW_STARTS)" },
        { "DLC04MQ05", "Power Play: 8 turns the gang the player snubbed on them and clears its raider settlements' owners (DLC04GangSetup, by DLC04EnemyGang); the gang is the chooser's in every game, as the stage carries the globals its scripts decide by (src/game/StageRules.h CARRIES: DLC04MQ04 500 and MQ05 5 the three ranks DLC04MakeEnemy reads, 8 the enemy gang and the parks' DLC04MQ03Claimed*, 20/120/255 the enemy gang)" },
        { "CZ", "Combat Zone: 100 puts Tommy's fighters on the player's side for the arena fight (390 takes them off), 250 (reached from 200-247) turns the arena's raiders on the player, 500 (and 510, which sets it) makes Cait available as a companion (AVAILABLE); 1 is the developers' quickstart (kMoves)" },
        { "FFGoodneighbor07", "the end of The Big Dig (MS16 260 and 270 set it): 30 turns Bobbi on the player (her turrets' fight with it), 80 makes Hancock available as a companion (AVAILABLE); 52's caps are Bobbi's, the player's take, both players' as a shared quest's reward" },
        { "DLC03MQ00", "Far Harbor's master quest: 0, its start-up, makes the island's factions allies of one another in every game; 100 is the Far Harbor kick-out (the town turned on the player, Pearl moved, its custom event to the town's and Acadia's quests), set by DLC03MQ06_WindFarmHandling 50" },
        { "DLC03CoA_DialogueNucleusEntrance", "the Nucleus's door: 50 turns its guards on the player, 150 sets DLC03CoA_Kickout 700, the Children of Atom turned on the player as the Nucleus's 70; the story's DLC03MQ06 and DLC03AtomM01 set it too (its stopping early is STOPS_EARLY_READ's)" },
        { "DLC01MasterQuest", "Automatron's master quest: 10, its start-up, makes the add-on's factions allies of one another in every game; 20 and 320 are the story's (DLC01MQ02 3000, DLC01MQ01 3000), 300 and 310 schematics the player can build; no bookkeeping, though its name is DENY_PATTERNS's" },
        { "DLC01Lair", "the Mechanist's lair: 1400 (reached from 1350, and set by the story's DLC01MQ05 885 too) turns the Mechanist's robots on the player, 2000 gives the player the lair's workshop; 3-9 are the developers' quickstarts (kMoves)" },
        { "DLC01Lair_ChangeLocation", "its 0 starts DLC01Lair (above) if it isn't running, and stops" },
        { "DLC04MS01SierraAttacks", "Sierra Petrovita turned on the player: 10 puts her in PlayerEnemyFaction and starts her fight" },
        { "DLC04DialogueHarvey", "Harvey: 200 turns him on the player; 1000, his leaving, sets the end of his roadside encounter (ALLOW_STARTS)" },
        { "DLC04DialogueHubologists", "the Hubologists: 200 turns them on the player; their other stages are each player's own sessions (kOwn rows, never set from a friend's): the rads cast on the player and the extraction chair's idle played on them (20-27), the paid donations (30-37), the donation's objective (40, 41, 100) and the rank's global and bonus spells (50-57)" },
    };

    // The ALLOW_OUTCOMES quests whose kick-out turns a companion on the player as part of its outcome, by "<the kick-out
    // quest> <the companion's quest>", with why: their walk lets a companion's affinity, a dismissal and a stage of that
    // companion's quest (no other's) through as well (kickout). The kick-out is shared (the maintainer's choice of
    // 2026-11-10 and 11), and a friend's game runs it as the chooser's did, the companion included: as in the game, a
    // Danse, Deacon or Gage who has followed this player is dismissed and turned by it, the friend's choice being both
    // players' (StageRules.h DISMISSES only warns the player first, once, when the companion is the one following them);
    // one who is the host's is theirs alone anyway (src/game/Companions).
    static readonly Dictionary<string, string> ALLOW_KICKOUTS = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "DLC04RaiderKickout DLC04COMGage", "20's DLC04SendKickOut, only for a Gage who has been the player's companion (HasBeenCompanionFaction): his affinity to -5000, DLC04COMGage 91 dismisses him with no settlement list and for good, and he joins the player's enemies, as the gangs do" },
        { "BoSKickOut COMDanse", "KickOut (20 and 30), only for a Danse who has been the player's companion (HasBeenCompanionFaction): his affinity to -5000, COMDanse 91 dismisses him for good; and he joins the player's enemies unless already exiled (BoSDansePostGame 10)" },
        { "DN084 COMDanse", "its quickstarts 2, 4, 6 and 8 set the Brotherhood's kick-out, whose KickOut reaches Danse as above" },
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

    // A stage of a quest every session shares by its type is followed too, unlike the other walks: the stage's fragment
    // sets it in this game the moment the stage lands, so the hold must stand at the stage itself (MQ302Post 10 sets
    // MQ302Min 2000, the ending's Institute kick-out: both are HOSTILE's, and whichever reaches a friend's game first waits).
    static readonly MoveScan.Walk hostile = new MoveScan.Walk { Direct = Hostile };

    // How a stage makes the player a faction's enemy (the first way found), or null.
    public static string StageHostile(int a_file, GearScan.Quest a_quest, int a_stage) { return hostile.Stage(a_file, a_quest, a_stage); }

    // ---- A developer's quickstart stage 0, which the game never runs in play ----------------------------------------------------
    // Many quests' stage 0 is a developer's shortcut into the middle of the quest: it moves the player to a test marker and sets
    // what a played-through game would have done by then (MS07's 0: MoveTo TestMQ201PlayerMarker, Nick made the companion;
    // BoSM04Postquest's 0: MoveTo the Prydwen's gantry, DN053's stages set). The game runs a stage only when something sets it
    // or, for a start-up stage, when the quest starts; so a stage 0 that is no start-up stage and that nothing in the game's
    // files sets never runs in a game played through, and the walk does not hold what it does against its quest (as RuleScan
    // leaves such a stage out of StageRules.h, and StageRules::Starting judges a starting stage without it). Skipped only when
    // all of these are read so in the game's files:
    // - stage 0 has no start-up flag (the quest's INDX), and its own fragment moves the player by MoveTo (the quickstart's mark;
    //   a stage 0 that doesn't is walked as any);
    // - no script attached anywhere in the official plugins (ParentScan.FILES: a record's own, a reference's and its base's, a
    //   fragment's, an alias's) that holds the quest in a property calls SetStage on it with 0 or a stage that is no literal,
    //   nor lets it out of reach (passes it to a call, stores it in a property, an array or a struct, returns it); the same
    //   for any function anywhere that reads such a property by its name off another script (propget);
    // - none of the quest's own scripts, its stage fragments or its aliases' scripts does so with self or GetOwningQuest();
    // - no fragment of a topic's info, a scene, a package, a perk or a terminal sets a stage of its own quest
    //   (GetOwningQuest) with 0 or a stage that is no literal, unless its name is another quest's topic info (TIF_<quest>_);
    // - no scene of the quest sets its stage 0 as a phase begins or ends (SCQS), and no function that runs may read the quest
    //   by its form ID (GetForm or GetFormFromFile with its ID, or with an ID that is no literal and a file that may be its
    //   plugin: FormRead);
    // - every such script is in the masters' archives, and every VMAD was read to its end.
    // A player's console can still set it: StageRules.h has a kDebug row for each one skipped here that a session shares, so
    // a friend's is never set in another game (StageRulesTests checks the rows against these).
    static string dataPath;
    // by quest (file, form ID): what its stage 0 does, for those skipped; why it is walked, for those that look like one
    static readonly Dictionary<Tuple<int, uint>, string> quickstarts = new Dictionary<Tuple<int, uint>, string>();
    static readonly Dictionary<Tuple<int, uint>, string> walkedStage0 = new Dictionary<Tuple<int, uint>, string>();

    // every script the game's data holds, in any archive or loose, by its name (Fragments:Terminals:TERM_...)
    static HashSet<string> shipped;

    // whether an archive is a general one (GNRL; a texture archive, DX10, holds no scripts)
    static bool General(string a_path) { using (var fs = File.OpenRead(a_path)) { var b = new byte[12]; return fs.Read(b, 0, 12) == 12 && Encoding.ASCII.GetString(b, 8, 4) == "GNRL"; } }

    // Whether a script is in none of the game's archives and not loose in Data\Scripts: a record that names it runs nothing.
    static bool Absent(string a_script)
    {
        if (shipped == null)
        {
            shipped = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            Func<string, string> name = p => { var n = p.Replace('/', '\\'); if (n.StartsWith(@"Scripts\", StringComparison.OrdinalIgnoreCase)) n = n.Substring(8); return n.Substring(0, n.Length - 4).Replace('\\', ':'); };
            foreach (var ba in Directory.GetFiles(dataPath, "*.ba2").Where(General))
                GearScan.Ba2(ba, n => { if (n.EndsWith(".pex", StringComparison.OrdinalIgnoreCase)) shipped.Add(name(n)); return false; });
            var loose = Path.Combine(dataPath, "Scripts");
            if (Directory.Exists(loose))
                foreach (var p in Directory.GetFiles(loose, "*.pex", SearchOption.AllDirectories)) shipped.Add(name(p.Substring(dataPath.Length).TrimStart('\\')));
        }
        return !shipped.Contains(a_script);
    }

    static bool IntLiteral(string a, out int a_value) { return int.TryParse(a, out a_value); }

    // The strings a variable read in a function may hold, or null when not all are known: a literal; a local or temp, from
    // every write to it (an assign or cast of such a value; unwritten, it holds ""); a variable of the function's script that
    // no script writes, from what it starts with. A parameter, a property (::Name_var: set where its script is attached) or
    // any other write is unknown.
    static List<string> Strings(GearScan.Func f, string v, int a_depth)
    {
        if (v.Length >= 2 && v.StartsWith("\"", StringComparison.Ordinal)) return new List<string> { v.Substring(1, v.Length - 2) };
        if (v == "None") return new List<string> { "" };
        if (a_depth > 8 || f.Params.Any(p => Is(p, v))) return null;
        if (f.Types.ContainsKey(v) || v.StartsWith("::temp", StringComparison.OrdinalIgnoreCase))
        {
            var list = new List<string> { "" };
            foreach (var i in f.Code)
                foreach (var d in MoveScan.Dests(i))
                {
                    if (!Is(i.Args[d], v)) continue;
                    if ((i.Op != 13 && i.Op != 14) || i.Args.Count < 2) return null;
                    var s = Strings(f, i.Args[1], a_depth + 1);
                    if (s == null) return null;
                    list.AddRange(s);
                }
            return list;
        }
        if (v.StartsWith("::", StringComparison.Ordinal) || MoveScan.VarWritten(f.Obj, v)) return null;
        var start = MoveScan.Default(f.Obj, v);
        return start != null && (start == "None" || start.StartsWith("\"", StringComparison.Ordinal)) ? Strings(f, start, a_depth + 1) : null;
    }

    // Whether an instruction may read the quest by its form ID, and how, or null: GetForm with the quest's ID (its low 24
    // bits: a DLC's load index is not known here) or one that is no literal; GetFormFromFile the same, unless every file name
    // it may be handed is known (Strings) and none is the quest's plugin. A function of these names on any script counts.
    static string FormRead(GearScan.Func f, GearScan.Instr i, uint a_id, string a_esm)
    {
        if (i.Op != 25 || i.Args.Count < 4) return null;
        var a = i.Args;
        bool fromFile = Is(a[1], "GetFormFromFile");
        if (!fromFile && !Is(a[1], "GetForm")) return null;
        int id;
        if (IntLiteral(a[3], out id) && ((uint)id & 0xFFFFFF) != (a_id & 0xFFFFFF)) return null;
        if (fromFile && a.Count > 4)
        {
            var files = Strings(f, a[4], 0);
            if (files != null && !files.Any(n => Is(n, a_esm))) return null;
        }
        return f.Obj + "." + f.Name + ": " + a[1] + "(" + string.Join(", ", a.Skip(3).ToArray()) + ")";
    }

    // How a function sets a stage 0 of the quest held in a_vars (its variables, properties and temps that hold it; grown as
    // it is copied, and by GetOwningQuest() with a_owning), or lets it out of reach: null when it does neither. a_names: the
    // property names that hold it (a propget of one, off any object, holds it). a_id: its form ID in its file. a_known: the
    // whole numbers its parameters, and its object's variables (::Name_var), hold as it starts; a stage it sets must be a
    // literal or one of these, and not 0. A call that is handed the quest is followed into every script function it may
    // run (Callees), with that parameter held and the whole numbers it is handed; a native one is harmless unless it sets a
    // stage on it or calls by name (CallFunction and the like), and what it returns is held; a call whose script is not
    // read lets it out. a_esm (the quest's plugin): what a form read by an ID that may be the quest's returns is held too
    // (FormRead).
    static string Sets0(GearScan.Func f, HashSet<string> a_vars, bool a_owning, HashSet<string> a_names, uint a_id,
        Dictionary<string, string> a_known = null, HashSet<string> a_seen = null, int a_depth = 0, string a_esm = null)
    {
        // Quest.SetStage is the stage setter itself (it hands its stage to the native SetCurrentStageID): what it sets is
        // read where it is called
        if (Is(f.Obj, "Quest") && Is(f.Name, "SetStage")) return null;
        if (a_seen == null) a_seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (a_known == null) a_known = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var held = new HashSet<string>(a_vars, StringComparer.OrdinalIgnoreCase);
        Func<string, bool> has = v => held.Contains(v);
        string where = f.Obj + "." + (string.IsNullOrEmpty(f.State) ? "" : f.State + ".") + f.Name + ": ";
        // the whole number a variable holds wherever it is read, or null: a literal; or every value it may hold is the
        // same, from what it starts with (a_known; a local or temp starts at 0) and each write (an assign or cast of such
        // a value)
        var writes = new Dictionary<string, List<GearScan.Instr>>(StringComparer.OrdinalIgnoreCase);
        foreach (var i in f.Code)
            foreach (var d in MoveScan.Dests(i))
            {
                List<GearScan.Instr> w;
                if (!writes.TryGetValue(i.Args[d], out w)) writes[i.Args[d]] = w = new List<GearScan.Instr>();
                w.Add(i);
            }
        Func<string, int, string> value = null;
        value = (v, depth) =>
        {
            int n;
            if (int.TryParse(v, out n)) return n.ToString();
            if (depth > 8) return null;
            string start;
            if (!a_known.TryGetValue(v, out start))
            {
                bool local = !f.Params.Any(p => Is(p, v)) && (!v.StartsWith("::", StringComparison.Ordinal) || v.StartsWith("::temp", StringComparison.OrdinalIgnoreCase));
                start = local ? "0" : null;
            }
            if (start == null) return null;
            List<GearScan.Instr> ws;
            if (!writes.TryGetValue(v, out ws)) return start;
            foreach (var i in ws)
            {
                if (i.Op != 13 && i.Op != 14) return null;
                if (value(i.Args[1], depth + 1) != start) return null;
            }
            return start;
        };
        // a call handed it (first: where the arguments start); native: it runs no script function
        bool native = false;
        Func<GearScan.Instr, int, string, string> handed = (i, first, what) =>
        {
            native = false;
            if (MoveScan.Dynamic(i.Args[i.Op == 25 ? 1 : 0])) return where + "passes it to " + what + ", which calls by name";
            List<GearScan.Func> targets;
            if (!MoveScan.Callees(f, i, out targets)) return where + "passes it to " + what + ", whose script is not read";
            native = targets.All(g => g.Code.Count == 0);
            bool same = i.Op == 24 || (i.Op == 23 && Is(i.Args[1], "self"));  // the same object: its variables as they are here
            for (int k = first; k < i.Args.Count; k++)
            {
                if (!has(i.Args[k])) continue;
                foreach (var g in targets)
                {
                    int p = k - first;
                    if (p >= g.Params.Count) continue;
                    var known = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    if (same) foreach (var kv in a_known) if (kv.Key.StartsWith("::", StringComparison.Ordinal)) known[kv.Key] = kv.Value;
                    for (int j = first; j < i.Args.Count && j - first < g.Params.Count; j++) { var v = value(i.Args[j], 0); if (v != null) known[g.Params[j - first]] = v; }
                    var seen = g.Obj + "." + g.State + "." + g.Name + "#" + p + ":" + string.Join(",", known.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value).ToArray());
                    if (!a_seen.Add(seen)) continue;
                    if (a_depth >= 12) return where + "passes it to " + what + ", too deep to follow";
                    var w = Sets0(g, new HashSet<string>(StringComparer.OrdinalIgnoreCase) { g.Params[p] }, false, new HashSet<string>(), a_id, known, a_seen, a_depth + 1, a_esm);
                    if (w != null) return where + "passes it to " + what + " < " + w;
                }
            }
            return null;
        };
        for (int pass = 0; pass < 2; pass++)  // twice: a loop's later copy reaches its earlier use
            foreach (var i in f.Code)
            {
                var a = i.Args;
                if ((i.Op == 13 || i.Op == 14) && a.Count >= 2) { if (has(a[1])) held.Add(a[0]); else if (pass == 0) held.Remove(a[0]); continue; }
                if (i.Op == 28 && a.Count >= 3) { if (a_names.Contains(a[0])) held.Add(a[2]); else if (pass == 0) held.Remove(a[2]); continue; }
                if (i.Op == 23 && a.Count >= 3)
                {
                    if (a_owning && Is(a[0], "GetOwningQuest")) { held.Add(a[2]); continue; }
                    if (System.Text.RegularExpressions.Regex.IsMatch(a[0], @"^Set\w*Stage", System.Text.RegularExpressions.RegexOptions.IgnoreCase) && has(a[1]))
                    {
                        var stage = a.Count < 4 ? null : value(a[3], 0);
                        if (stage == null || stage == "0") return where + a[0] + "(" + (a.Count < 4 ? "?" : Bare(a[3])) + ")";
                    }
                    if (Is(a[0], "CastAs") && has(a[1])) { held.Add(a[2]); continue; }
                    if (Enumerable.Range(3, Math.Max(0, a.Count - 3)).Any(k => has(a[k]))) { var w = handed(i, 3, a[0]); if (w != null) return w; if (native) { held.Add(a[2]); continue; } }
                    if (pass == 0) held.Remove(a[2]);
                    continue;
                }
                if (i.Op == 24 && a.Count >= 2)
                {
                    if (Enumerable.Range(2, Math.Max(0, a.Count - 2)).Any(k => has(a[k]))) { var w = handed(i, 2, "parent." + a[0]); if (w != null) return w; if (native) { held.Add(a[1]); continue; } }
                    if (pass == 0) held.Remove(a[1]);
                    continue;
                }
                if (i.Op == 25 && a.Count >= 3)
                {
                    if (a_esm != null && FormRead(f, i, a_id, a_esm) != null) { held.Add(a[2]); continue; }
                    if (Enumerable.Range(3, Math.Max(0, a.Count - 3)).Any(k => has(a[k]))) { var w = handed(i, 3, a[0] + "." + a[1]); if (w != null) return w; if (native) { held.Add(a[2]); continue; } }
                    if (pass == 0) held.Remove(a[2]);
                    continue;
                }
                if (i.Op == 26 && a.Count >= 1 && has(a[0])) return where + "returns it";
                if ((i.Op == 29 || i.Op == 33 || i.Op == 39) && a.Count >= 3 && has(a[2])) return where + "stores it (" + GearScan.OPS[i.Op] + ")";
                if ((i.Op == 42 || i.Op == 43) && a.Count >= 2 && has(a[1])) return where + "stores it (" + GearScan.OPS[i.Op] + ")";
            }
        return null;
    }

    // The whole numbers an attached script's variables hold for good (Sets0's a_known): those given where it is attached
    // that no script changes.
    static Dictionary<string, string> Constant(string a_script, Dictionary<string, string> a_given)
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var kv in a_given) if (!MoveScan.VarWritten(a_script, kv.Key)) d[kv.Key] = kv.Value;
        return d;
    }

    // What may set the quest's stage 0 in a game played through (the first found), or null: nothing in the game's files
    // does (the checks listed above). a_read: how many scripts were read for it.
    static string Stage0SetBy(GearScan.Quest q, int a_file, out int a_read)
    {
        a_read = 0;
        ParentScan.LoadRecords(dataPath);
        List<string> unread;
        if (ParentScan.VmadErrors(out unread) > 0) return "VMADs not read to their end: " + string.Join("; ", unread.ToArray());
        var key = ParentScan.KeyOf(a_file, q.Id);
        var none = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // the scripts that hold it in a property, and every function that reads one of those properties off another script
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var h in ParentScan.Holders(key))
        {
            var funcs = MoveScan.Functions(h.Item2);
            if (funcs == null && Absent(h.Item2)) continue;  // runs nothing
            if (funcs == null) return h.Item1 + ": " + h.Item2 + " is not in the masters' archives";
            var vars = new HashSet<string>(h.Item3.SelectMany(n => new[] { n, "::" + n + "_var" }), StringComparer.OrdinalIgnoreCase);
            foreach (var n in h.Item3) names.Add(n);
            var known = Constant(h.Item2, h.Item4);
            foreach (var f in funcs.Where(MoveScan.Runs)) { var w = Sets0(f, vars, false, none, q.Id, known); if (w != null) return h.Item1 + ": " + w; }
            a_read++;
        }
        foreach (var f in MoveScan.AllFunctions())
            if (MoveScan.Runs(f) && f.Code.Any(i => i.Op == 28 && i.Args.Count >= 3 && names.Contains(i.Args[0]) && !Is(i.Args[1], "self")))
            {
                var w = Sets0(f, none, false, names, q.Id);
                if (w != null) return "a property that holds it, read off another script: " + w;
            }
        // its own scripts, its stage fragments and its aliases' scripts
        foreach (var o in ParentScan.OnQuest(key))
        {
            var funcs = MoveScan.Functions(o.Item2);
            if (funcs == null && Absent(o.Item2)) continue;  // runs nothing
            if (funcs == null) return o.Item1 + ": " + o.Item2 + " is not in the masters' archives";
            var self = o.Item3 ? new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "self" } : none;
            var known = Constant(o.Item2, o.Item4);
            foreach (var f in funcs.Where(MoveScan.Runs)) { var w = Sets0(f, self, true, none, q.Id, known); if (w != null) return o.Item1 + ": " + w; }
            a_read++;
        }
        // the fragments whose GetOwningQuest() is a quest not read here, but by a topic info's name
        foreach (var o in ParentScan.OtherFragments())
        {
            var funcs = MoveScan.Functions(o.Item2);
            // one in no archive runs nothing, and a terminal's or a perk's fragment has no owning quest (GetOwningQuest is an alias's, a topic info's, a scene's
            // or a package's): what one that holds the quest in a property does is read above
            if (funcs == null && (Absent(o.Item2) || o.Item1.StartsWith("TERM ", StringComparison.Ordinal) || o.Item1.StartsWith("PERK ", StringComparison.Ordinal))) continue;
            if (funcs == null) return o.Item1 + ": " + o.Item2 + " is not in the masters' archives";
            foreach (var f in funcs.Where(MoveScan.Runs))
            {
                var w = Sets0(f, none, true, none, q.Id, Constant(o.Item2, o.Item3));
                if (w == null) continue;
                var m = System.Text.RegularExpressions.Regex.Match(o.Item2, @"(?:^|:)TIF_(\w+)_[0-9A-Fa-f]{8}$");
                if (m.Success && !Is(m.Groups[1].Value, q.Edid)) continue;  // another quest's topic info
                return o.Item1 + ": " + w;
            }
        }
        // a script that may read it by its form ID (any function that runs: those Sets0 follows too): by its own ID, at
        // all; by an ID or a file that is no literal, unless what the read returns is followed as the quest (Sets0) and
        // neither sets its stage 0 nor gets out of reach
        var esm = GearScan.MASTERS[a_file].Esm;
        foreach (var f in MoveScan.AllFunctions().Where(MoveScan.Runs))
        {
            string unsure = null;
            foreach (var i in f.Code)
            {
                var w = FormRead(f, i, q.Id, esm);
                if (w == null) continue;
                int id;
                if (IntLiteral(i.Args[3], out id)) return w;
                unsure = unsure ?? w;
            }
            if (unsure == null) continue;
            var s = Sets0(f, none, false, none, q.Id, null, null, 0, esm);
            if (s != null) return unsure + ", and " + s;
        }
        foreach (var sc in ParentScan.SceneStages(key))
            if (sc.Item2.Contains(0)) return sc.Item1 + " sets stage 0 as a phase begins or ends";
        return null;
    }

    // Whether the walk leaves out the quest's stage 0: a developer's quickstart the game never runs in play (above).
    static bool QuickStart(GearScan.Quest q, int a_file)
    {
        var key = Tuple.Create(a_file, q.Id & 0xFFFFFF);
        if (quickstarts.ContainsKey(key)) return true;
        if (walkedStage0.ContainsKey(key) || q.Startup.Contains(0)) return false;
        List<GearScan.Func> list;
        if (!MoveScan.Fragments(a_file, q.Id).TryGetValue(0, out list)) return false;
        var move = list.Select(MoveScan.MovesItself).FirstOrDefault(m => m != null && m.StartsWith("MoveTo", StringComparison.Ordinal));
        if (move == null) return false;
        int read;
        var by = Stage0SetBy(q, a_file, out read);
        if (by != null) { walkedStage0[key] = move + "; it may be set: " + by; return false; }
        quickstarts[key] = move + " (" + read + " scripts that hold the quest or are its own read)";
        return true;
    }

    // The quests whose stage 0 the walk left out, by (file, form ID), with what it does: StageRules.h's kDebug rows are
    // these, for the quests a session shares.
    public static IDictionary<Tuple<int, uint>, string> QuickStarts { get { return quickstarts; } }
    public static bool IsQuickStart(GearScan.Quest q, int a_file) { return QuickStart(q, a_file); }

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
        if ((q.Flags & 0x101) == 0 && !RunsOnce(q, a_file)) return "repeatable";
        var aliases = new Dictionary<int, GearScan.Alias>();
        foreach (var a in q.Aliases) if (!aliases.ContainsKey(a.Id)) aliases[a.Id] = a;
        if (!ALLOW_FILLS.ContainsKey(q.Edid) && q.Aliases.Any(a => PerGame(aliases, a.Id, 0))) return "an alias each game fills";
        var outcomes = ALLOW_OUTCOMES.ContainsKey(q.Edid);
        foreach (var stage in MoveScan.Fragments(a_file, q.Id).Keys.OrderBy(k => k))
        {
            if (skipped.Contains(Tuple.Create(a_file, local, stage))) continue;  // never run in a friend's game: a payment (kPays) or its companion's owner's (kEach); the walks skip them too
            if (stage == 0 && QuickStart(q, a_file)) continue;
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
    // ambient. The six that teleport the player or their companion, or run a timer of their own, were here up to 0.16.1
    // and are SILENT's now, judged by StageRules: RRAct3Pickup (its 1 and 2, quickstarts into the Institute by MQ206Min
    // and MQ206BoS 1500, kMoves), InstituteTeleportEffects (its 10 moves X6-88 to the relay beside the player, kMoves),
    // DN136_Attack (its 10, the Brotherhood's route, teleports the player's companion and Dogmeat into the Institute,
    // kMoves; its 2000 sets BoSMarshal, ALLOW_STARTS), DNPrime_BoS304 (moves Prime and the synths, never the player; its
    // 15 starts DN035_EasyCityRadioOverrides and sets BoSMarshal, ALLOW_STARTS), MQ302Post (moves the faction leaders
    // home after the ending, never the player; Preston only when he is no companion here) and FFGoodneighbor08 (its 25
    // starts a two-hour timer whose end, 200, stops the event where this game's player is away from Goodneighbor:
    // StageRules HOST_TIMERS, the host's timer decides for the session).
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
        { "DN151_Tracking", "the tracker of DN151, a Minutemen request each game picks itself (PER_GAME)" },
        { "FFGoodneighbor01MemorySequence", "the memory lounger: fades out, takes the controls and hides the player" },
        { "GenericGoHome", "a tool other quests' scripts use for their own actors" },
        { "InstituteFirstVisitQuest", "each player's own first visit" },
        { "MS04PostQuest", "Kent's costume upgrades take and give the player's Silver Shroud gear, and pay caps" },
        { "MS07cGreetingHandler", "a companion's (Nick's greeting)" },
        { "MS07NickHateHandler", "a companion's (Nick's affinity)" },
        { "MinutemenCentralQuest", "the player's own Minutemen: settlements, radio, recruits" } };

    // Why an objective-less quest is not in SILENT ("" it is).
    static string WhySilent(GearScan.Quest q, int a_file)
    {
        if (q.Stages == 0) return "no objectives, no stages";
        var bare = System.Text.RegularExpressions.Regex.Replace(q.Edid, @"^DLC\d+_?", "");
        var outcomes = ALLOW_OUTCOMES.ContainsKey(q.Edid);  // read by hand: its name is no reason (DLC01MasterQuest is no bookkeeping)
        foreach (var d in DENY_PATTERNS)
            if (!outcomes && System.Text.RegularExpressions.Regex.IsMatch(bare, d.Item1)) return "denied by name: " + d.Item2;
        string why;
        if (DENY_SILENT.TryGetValue(q.Edid, out why)) return "denied by hand: " + why;
        why = StopsEarly(q, a_file);
        if (why != null && !STOPS_EARLY_READ.ContainsKey(q.Edid)) return STOPS_EARLY + why;
        return "";
    }

    const string STOPS_EARLY = "stops early: ";

    // The quests StopsEarly finds that were read by hand and lose nothing that matters when a joiner's catch-up stops them
    // early, with why: shared all the same.
    static readonly Dictionary<string, string> STOPS_EARLY_READ = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
        { "DLC03CoA_DialogueNucleusEntrance", "250 stops it, below 750, a line at the Nucleus's door that ends the talks there and sets 250 itself: caught up, 250 comes first and 750 is dropped, so the joiner's two door guards keep their SetProtected and the Richter scene's trigger stays enabled, a trigger whose scene can't run once the quest (run once) has stopped. Live, 750 is set and sets 250 as in the friend's game" },
    };

    // A stage whose own fragment stops the quest (Stop on itself) below a stage whose fragment does more than stop it, as
    // "<stop> stops it, below <stage>", or null. A friend catching up is sent the host's stages done before the session
    // in ascending order (the game keeps no order of them), and once a silent quest ran and stopped in a game, the rest
    // of it is dropped there: MS07Intro's 350 would stop it before its 400 starts Nick's case files there. Only the
    // fragment's own code is read; a stop through a function it calls is not seen. A stage that fails or completes the
    // quest's objectives (it has none) and stops it is a stop too (RRAct3Pickup's 9000 above its 300): dropped after the
    // other, nothing is lost.
    // A stage a friend's game never sets (src/game/StageRules.h HOST_TIMERS, read by Load when given the header: the host's
    // timer's, FFGoodneighbor08's 300; its rows with taken true, the shutdown stage every game takes, are not these)
    // cannot stop it there.
    static readonly HashSet<Tuple<int, uint, int>> hostTimers = new HashSet<Tuple<int, uint, int>>();

    static string StopsEarly(GearScan.Quest q, int a_file)
    {
        Func<GearScan.Instr, bool> stop = i => i.Op == 23 && Is(i.Args[0], "Stop") && (Is(i.Args[1], "self") || Is(i.Args[1], "kmyQuest") || Is(i.Args[1], "__temp"));
        Func<GearScan.Instr, bool> objectives = i => i.Op == 23 && (Is(i.Args[0], "FailAllObjectives") || Is(i.Args[0], "CompleteAllObjectives"));
        Func<GearScan.Instr, bool> call = i => (i.Op == 23 || i.Op == 24 || i.Op == 25) && !objectives(i);
        var stages = MoveScan.Fragments(a_file, q.Id);
        foreach (var s in stages.Where(kv => kv.Value.Any(f => f.Code.Any(stop)) && !hostTimers.Contains(Tuple.Create(a_file, q.Id & 0xFFFFFF, kv.Key))).Select(kv => kv.Key).OrderBy(k => k))
            foreach (var t in stages.Where(kv => kv.Key > s && kv.Value.Any(f => f.Code.Any(i => call(i) && !stop(i)))).Select(kv => kv.Key).OrderBy(k => k))
                return s + " stops it, below " + t;
        return null;
    }

    // Makes the SAFE set (QuestScan.Safe) and the SILENT one (QuestScan.Silent), less DENIED, and returns each quest's
    // verdict, by file and form ID: "" SAFE, "silent" SILENT.
    public static SortedDictionary<Tuple<int, uint>, string> Load(string data, string a_questFilter, string a_radiantRules, string a_stageRules)
    {
        MoveScan.Load(data);
        hostTimers.Clear();
        if (a_stageRules != null)
        {
            var text = File.ReadAllText(a_stageRules);
            int at = text.IndexOf("HOST_TIMERS[] = {", StringComparison.Ordinal);
            if (at < 0) throw new Exception("HOST_TIMERS not found in " + a_stageRules);
            var body = text.Substring(at, text.IndexOf("};", at, StringComparison.Ordinal) - at);
            var rows = System.Text.RegularExpressions.Regex.Matches(body, @"\{\s*(\d+),\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*(true|false)\s*\}");
            if (rows.Count == 0) throw new Exception("no HOST_TIMERS row matched { file, 0xQUEST, stage, true|false } in " + a_stageRules + ": the row format changed?");
            foreach (System.Text.RegularExpressions.Match m in rows)
                if (m.Groups[4].Value == "false")
                    hostTimers.Add(Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16), int.Parse(m.Groups[3].Value)));
        }
        QuestScan.Safe = null;
        QuestScan.Silent = null;
        skipped.Clear();
        skipped.UnionWith(Pays(a_stageRules, "kPays"));
        skipped.UnionWith(Pays(a_stageRules, "kEach"));
        skipped.UnionWith(SKIPPED_BY_HAND.Keys);
        var radiants = new HashSet<Tuple<int, uint>>(System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(a_radiantRules), "Entry\\{\\s*(\\d),\\s*0x([0-9A-Fa-f]+),\\s*\"")
            .Cast<System.Text.RegularExpressions.Match>().Select(m => Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16) & 0xFFFFFF)));
        var perGame = Table(a_questFilter, "PER_GAME");
        runsOnce = Table(a_questFilter, "RUNS_ONCE");
        dataPath = data;
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
    // "RULES[] = {" to its "};". kOwn (a stage each player has alone, never set from a friend's) is kPays's bit, and
    // counts as it: one reached from a shared stage would run here all the same.
    public static HashSet<Tuple<int, uint, int>> Pays(string a_stageRules, string a_flag = "kPays")
    {
        var text = File.ReadAllText(a_stageRules);
        int at = text.IndexOf("RULES[] = {", StringComparison.Ordinal);
        if (at < 0) throw new Exception("RULES not found in " + a_stageRules);
        var body = text.Substring(at, text.IndexOf("};", at, StringComparison.Ordinal) - at);
        var rows = new HashSet<Tuple<int, uint, int>>();
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(body, @"\{\s*(\d+),\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*([^,}]+),\s*\d+\s*\}"))
            if (m.Groups[4].Value.Contains(a_flag) || (a_flag == "kPays" && m.Groups[4].Value.Contains("kOwn"))) rows.Add(Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16), int.Parse(m.Groups[3].Value)));
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
    // inventory (Payment), or, of a quest no friend's stage is set on (whose stages RuleScan never judged), whose own
    // fragment takes any item as RuleScan's kPays check reads one (Unjudged: DN049 711's pre-war money, had it no row),
    // is listed, with how it is reached (the first way found for each stage). The walk does not
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
                : a_free.Contains(Tuple.Create(file, q.Id & 0xFFFFFF, stage)) ? q.Edid + " " + stage + ": kFree"
                : QuestScan.Shared(q, file) == "not shared" ? Unjudged(file, q, stage) : null
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

    // What a stage of a quest no friend's stage is set on takes from the player in its own fragment, as RuleScan's kPays
    // check reads a shared stage's (QuestScan.Takes: any item read from a property, the inventory emptied; not one an
    // alias's GetRef or another call put in a temp, the quest's own), or null. Its rows were never read by hand, as the
    // shared quests' were for kPays (a quest item there has no row), so any such take is a payment the walk lists.
    static string Unjudged(int a_file, GearScan.Quest a_quest, int a_stage)
    {
        var takes = QuestScan.Takes(new GearScan.Func { Code = MoveScan.FragmentCode(a_file, a_quest.Id, a_stage) })
            .Where(t => t.Item2 == "" || !t.Item2.StartsWith("temp", StringComparison.OrdinalIgnoreCase))
            .Select(t => t.Item1 + (t.Item2 == "" ? "" : " " + t.Item2) + t.Item3).Distinct().ToArray();
        return takes.Length == 0 ? null : a_quest.Edid + " " + a_stage + ": takes " + string.Join(", ", takes);
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
        sb.AppendLine(string.Format("# fills taken as fixed (ALLOW_FILLS): {0}", ALLOW_FILLS.Count));
        foreach (var kv in ALLOW_FILLS)
        {
            string verdict = null;
            foreach (var v in a_verdicts) if (MoveScan.perFile[v.Key.Item1][v.Key.Item2].Edid.Equals(kv.Key, StringComparison.OrdinalIgnoreCase)) verdict = v.Value;
            sb.AppendLine(string.Format("#   {0} ({1}): {2}", kv.Key, verdict == null ? "not found" : verdict == "" ? "SAFE" : verdict == "silent" ? "SILENT" : "left out, " + verdict, kv.Value));
        }
        sb.AppendLine(string.Format("# stopping early read by hand (STOPS_EARLY_READ): {0}", STOPS_EARLY_READ.Count));
        foreach (var kv in STOPS_EARLY_READ) sb.AppendLine(string.Format("#   {0}: {1}", kv.Key, kv.Value));
        sb.AppendLine(string.Format("# kick-outs that turn a companion on the player too (ALLOW_KICKOUTS): {0}", ALLOW_KICKOUTS.Count));
        foreach (var kv in ALLOW_KICKOUTS) sb.AppendLine(string.Format("#   {0}: {1}", kv.Key, kv.Value));
        sb.AppendLine(string.Format("# starts let through (ALLOW_STARTS): {0}", ALLOW_STARTS.Count));
        foreach (var kv in ALLOW_STARTS) sb.AppendLine(string.Format("#   {0}: {1}", kv.Key, kv.Value));
        return sb.ToString();
    }

    // SAFE's rows, SILENT's, then what was left out and why.
    public static string Run(string data, string a_questFilter, string a_radiantRules, string a_stageRules)
    {
        var verdicts = Load(data, a_questFilter, a_radiantRules, a_stageRules);
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
        // what the quickstart rule, RUNS_ONCE and ALLOW_STARTS let through, each with the quest's verdict
        Func<Tuple<int, uint>, string> verdict = k => !verdicts.ContainsKey(k) ? "?" : verdicts[k] == "" ? "SAFE" : verdicts[k] == "silent" ? "SILENT" : "left out, " + verdicts[k];
        sb.AppendLine(string.Format("# dynamic calls whose function name is not a literal (each makes every function count as one that runs): {0}{1}", MoveScan.DynamicNames.Count, MoveScan.DynamicNames.Count > 0 ? " (" + string.Join(", ", MoveScan.DynamicNames.Take(8).ToArray()) + ")" : ""));
        sb.AppendLine(string.Format("# a developer's quickstart stage 0, not walked (no start-up stage, its fragment moves the player, nothing in the game's files sets it; StageRules.h kDebug): {0}", quickstarts.Count));
        foreach (var kv in quickstarts.OrderBy(kv => kv.Key))
            sb.AppendLine(string.Format("#   {0} 0x{1:X6} {2} ({3}): {4}", kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid, verdict(kv.Key), kv.Value));
        foreach (var kv in walkedStage0.OrderBy(kv => kv.Key))
            sb.AppendLine(string.Format("# a stage 0 that moves the player, walked: {0} 0x{1:X6} {2} ({3}): {4}", kv.Key.Item1, kv.Key.Item2, MoveScan.perFile[kv.Key.Item1][kv.Key.Item2].Edid, verdict(kv.Key), kv.Value));
        sb.AppendLine(string.Format("# run once all the same (QuestFilter.h RUNS_ONCE): {0}", runsOnce.Count));
        foreach (var k in runsOnce.OrderBy(k => k))
        {
            GearScan.Quest q;
            sb.AppendLine(string.Format("#   {0} 0x{1:X6} {2} ({3})", k.Item1, k.Item2, k.Item1 < MoveScan.perFile.Count && MoveScan.perFile[k.Item1].TryGetValue(k.Item2, out q) ? q.Edid + string.Format(", flags {0:X4}", q.Flags) : "not found", verdict(k)));
        }
        sb.AppendLine(string.Format("# starts let through (ALLOW_STARTS): {0}", ALLOW_STARTS.Count));
        foreach (var kv in ALLOW_STARTS)
        {
            var from = verdicts.Keys.FirstOrDefault(k => Is(MoveScan.perFile[k.Item1][k.Item2].Edid, kv.Key.Split(' ')[0]));
            sb.AppendLine(string.Format("#   {0} ({1}): {2}", kv.Key, from == null ? "not found" : verdict(from), kv.Value));
        }
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
        foreach (var kv in SKIPPED_BY_HAND) sb.AppendLine(string.Format("# left unjudged by hand: {0}", kv.Value));
        sb.Append(SetsEach(a_questFilter, a_stageRules));
        return sb.ToString();
    }

    // The shared stages (of every quest a friend's stage is set on: those shared by their type (QuestScan.ReachesFriend), SAFE's,
    // SILENT's and the companions' (COMPANION_QUESTS)) that aren't kEach themselves and set, in their own fragment or through
    // the functions and stages it reaches (walked as -Moves walks them), a kEach stage of a quest with a driver (StageRules.h
    // DRIVERS): each with how it gets there, then the count, StageRules.h's SHARED_SETS_EACH (tests/StageRulesTests.cpp checks
    // it is 0). Such a stage would run the owner's stage with its script in a friend's game, and QuestSync's fresh-stage rule
    // would take that game for the quest's driver: MS07c 305 sets MS07cPostQuest 5 so (that quest stays unshared) and MQ204 200
    // sets MQ202NickPostSurgery 95 (so its 95 and 100 are plain).
    static string SetsEach(string a_questFilter, string a_stageRules)
    {
        var drivers = new HashSet<Tuple<int, uint>>();
        var text = File.ReadAllText(a_stageRules);
        int at = text.IndexOf("DRIVERS[] = {", StringComparison.Ordinal);
        if (at < 0) throw new Exception("DRIVERS not found in " + a_stageRules);
        var body = text.Substring(at, text.IndexOf("};", at, StringComparison.Ordinal) - at);
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(body, @"\{\s*(\d+),\s*0x([0-9A-Fa-f]+),\s*0x"))
            drivers.Add(Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16)));
        var each = Pays(a_stageRules, "kEach");
        var targets = new HashSet<Tuple<int, uint, int>>(each.Where(e => drivers.Contains(Tuple.Create(e.Item1, e.Item2))));
        var companions = Table(a_questFilter, "COMPANION_QUESTS");
        var walk = new MoveScan.Walk {
            Direct = c => null,
            Reaches = (file, q, stage) => targets.Contains(Tuple.Create(file, q.Id & 0xFFFFFF, stage)) ? q.Edid + " " + stage + ": the owner's (kEach)" : null };
        var hits = new List<string>();
        int walked = 0;
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var q in MoveScan.perFile[file].Values)
            {
                var key = Tuple.Create(file, q.Id & 0xFFFFFF);
                if (!(QuestScan.ReachesFriend(q, file) || QuestScan.Safe.Contains(key) || QuestScan.Silent.Contains(key) || companions.Contains(key))) continue;
                foreach (var stage in MoveScan.Fragments(file, q.Id).Keys.OrderBy(k => k))
                {
                    if (each.Contains(Tuple.Create(file, key.Item2, stage))) continue;
                    walked++;
                    var r = walk.Stage(file, q, stage);
                    if (r != null) hits.Add(string.Format("#   {0} 0x{1:X6} {2} {3}: {4}", file, key.Item2, q.Edid, stage, r));
                }
            }
        var sb = new StringBuilder();
        foreach (var h in hits) sb.AppendLine(h);
        sb.AppendLine(string.Format("# shared stages that set an owner's stage (a kEach row of a quest with a driver; {0} stages walked): {1} (StageRules.h SHARED_SETS_EACH)", walked, hits.Count));
        return sb.ToString();
    }
}

// The companions' quests a session shares as side quests (src/game/QuestFilter.h's COMPANION_QUESTS, read from it), every
// quest with a kEach row in src/game/StageRules.h's RULES (Nick's and Cait's, shared by their type or as SAFE's) and any
// given with -Quests file:hex: every stage of theirs with what its own fragment does, and RULES rows for them. A stage is kEach when
// its fragment does what is the companion's owner's alone: who is or can be a companion (SafeScan's COMPANIONS), their
// affinity (an argument or call named *Affinity*, CA_*), their keywords, perks or factions (AddKeyword, RemoveKeyword,
// AddPerk, RemovePerk, AddToFaction, RemoveFromFaction, SetFactionRank on an actor that isn't the player), a stage of
// another companion quest (COM*, Followers), or a call into the quest's own script (kmyQuest: a timer, the companion's
// state), each listed for a look by hand: what a friend's game does instead (EACH: the objectives it shows and
// completes, the world's half, the quest's XP and completion) is written by hand, from the calls listed. kMoves and
// kPays are -Rules' (MoveScan, QuestScan.Takes); a stage whose scene runs on aliases a kEach stage changed is kEach by
// hand (Curie's 400).
public static class CompanionScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }

    static readonly HashSet<string> COMPANIONS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "SetCompanion", "DismissCompanion", "AddToPlayerFaction", "SetAvailableToBeCompanion", "SetPlayerTeammate", "AllowCompanion",
        "DisallowCompanion", "ClearCompanion", "ReleaseCompanion", "SetDogmeatCompanion", "DismissDogmeatCompanion", "ClearDogmeatCompanion", "SetAffinity" };
    static readonly HashSet<string> TRAITS = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "AddKeyword", "RemoveKeyword", "AddPerk", "RemovePerk", "AddToFaction", "RemoveFromFaction", "SetFactionRank" };

    // What of a fragment is its companion owner's, as "why" strings (none: it is set as any shared stage), and the calls
    // it makes (for EACH by hand).
    static List<string> Owners(GearScan.Func f, List<string> a_calls)
    {
        var why = new List<string>();
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var i in f.Code)
        {
            foreach (var a in i.Args)
            {
                var b = Bare(a);
                if (b.IndexOf("Affinity", StringComparison.OrdinalIgnoreCase) >= 0 || b.StartsWith("CA_", StringComparison.OrdinalIgnoreCase) || b.StartsWith("pCA_", StringComparison.OrdinalIgnoreCase))
                { why.Add("affinity " + b); break; }
            }
            if (i.Op == 25 && Is(i.Args[0], "game") && Is(i.Args[1], "GetPlayer")) { players.Add(i.Args[2]); continue; }
            if (i.Op != 23 && i.Op != 25) continue;
            var m = i.Op == 25 ? i.Args[1] : i.Args[0];
            var on = i.Op == 25 ? i.Args[0] : Bare(i.Args[1]);
            a_calls.Add(m + "(" + on + (i.Args.Count > 3 ? ", " + string.Join(", ", i.Args.Skip(3).Select(Bare).ToArray()) : "") + ")");
            if (SafeScan.AllowsOnly(i)) continue;  // AllowCompanion(actor, False, False): allowed again, nobody recruited; a friend's copy only
            if (COMPANIONS.Contains(m)) why.Add("companion " + m);
            else if (TRAITS.Contains(m) && !players.Contains(i.Args[1])) why.Add("traits " + m + " " + string.Join(" ", i.Args.Skip(3).Select(Bare).ToArray()));
            else if (Is(m, "SetStage") && System.Text.RegularExpressions.Regex.IsMatch(Bare(i.Args[1]), "^p?(COM|Followers)", System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                why.Add("companion SetStage " + Bare(i.Args[1]));
            else if (i.Op == 23 && Is(i.Args[1], "kmyQuest")) why.Add("its quest's script " + m);
            if (i.Op == 23) players.Remove(i.Args[2]);
        }
        return why;
    }

    // Stages decided by hand, with why (the scan's own verdict is printed beside it).
    static readonly Dictionary<Tuple<int, uint, int>, Tuple<string, string>> HAND = new Dictionary<Tuple<int, uint, int>, Tuple<string, string>> {
        // Nick's Long Time Coming: his dismissal (DisallowCompanion's DismissCompanion returns unless he is this game's companion), the
        // affinity events (to this game's current companion, not Nick in a friend's game) and the quest's own script (the fight's
        // timers, the raiders) touch a friend's copy of him and the world: the world's half is the script's.
        { Tuple.Create(0, 0x0229FEu, 10), Tuple.Create("plain", "the holotapes, the terminals seen (its quest's script): the world's") },
        { Tuple.Create(0, 0x0229FEu, 200), Tuple.Create("plain", "the station's raiders' faction: the world's") },
        { Tuple.Create(0, 0x0229FEu, 205), Tuple.Create("plain", "the tapes taken (its quest's script, in each game from its own player)") },
        { Tuple.Create(0, 0x0229FEu, 210), Tuple.Create("plain", "Nick runs to Eddie's bunker: DisallowCompanion dismisses him only where he is the companion; Eddie, the triggers and the raiders are the world's") },
        { Tuple.Create(0, 0x0229FEu, 220), Tuple.Create("plain", "the bunker fight's factions and door timer: the world's (kMoves: the controls)") },
        { Tuple.Create(0, 0x0229FEu, 230), Tuple.Create("plain", "the bunker door, the timer cancelled: the world's") },
        { Tuple.Create(0, 0x0229FEu, 233), Tuple.Create("plain", "the raiders' combat stopped: the world's") },
        { Tuple.Create(0, 0x0229FEu, 240), Tuple.Create("plain", "the raiders' faction, Eddie on the player: the world's") },
        { Tuple.Create(0, 0x0229FEu, 250), Tuple.Create("plain", "Nick's fight faction, the trap door; the affinity event goes to this game's companion") },
        { Tuple.Create(0, 0x0229FEu, 260), Tuple.Create("plain", "the raiders' faction, the objectives, his timer to the grave; the affinity bump is on this game's copy of him") },
        { Tuple.Create(0, 0x0229FEu, 280), Tuple.Create("plain", "an affinity event to this game's companion") },
        { Tuple.Create(0, 0x0229FEu, 290), Tuple.Create("plain", "AllowCompanion(Nick, False, False): allowed again, a friend's copy only; the objectives") },
        { Tuple.Create(0, 0x0229FEu, 300), Tuple.Create("plain", "the same, and MS07cPostQuest 100") },
        { Tuple.Create(0, 0x0229FEu, 305), Tuple.Create("plain", "the same, and MS07cPostQuest 5") },
        // Cait: her chair would take her from a friend she followed, her cure would give her to a friend with no companion.
        { Tuple.Create(0, 0x1666C5u, 60), Tuple.Create("kEach", "DisallowCompanion(Cait, True): dismissed for the chair (DisallowedCompanionFaction, the driving game's mark until 255); her chair scene is the owner's") },
        { Tuple.Create(0, 0x1666C5u, 255), Tuple.Create("kEach", "AllowCompanion(Cait, True, False): SetCompanion where none follows; her chem conditions and affinity global the owner's; EACH: objective 50, the quest completed") },
        // Danse: nothing recruits or dismisses; the Brotherhood's ending is the world's.
        { Tuple.Create(0, 0x0B9F9Du, 10), Tuple.Create("plain", "Danse out of the Brotherhood's faction into the bunker's: the world's") },
        { Tuple.Create(0, 0x0B9F9Du, 25), Tuple.Create("plain", "Haylen placed (its quest's script): the world's") },
        { Tuple.Create(0, 0x0B9F9Du, 125), Tuple.Create("plain", "Maxson's keyword, the vertibird: the world's") },
        { Tuple.Create(0, 0x0B9F9Du, 255), Tuple.Create("plain", "AllowCompanion(Danse, False, False): allowed again, a friend's copy only; the ending's quests, his X-01, the achievement: the world's") },
        // Nick after the Memory Den: only the forced recruit is the owner's.
        { Tuple.Create(0, 0x0FAC86u, 90), Tuple.Create("kEach", "AllowCompanion(Nick, True, True): ForceCompanion, recruited to whoever runs it; then its 100") },
        { Tuple.Create(0, 0x0FAC86u, 95), Tuple.Create("plain", "the objective hidden, then its 100; the story's MQ204 200 sets it in every game (SafeScan.SetsEach)") },
        { Tuple.Create(0, 0x0FAC86u, 100), Tuple.Create("plain", "AllowCompanion(Nick, False, False): allowed again; his affinity conditions, which MQ204 200 sets in every game through 95 anyway (SKIPPED_BY_HAND); the objective, Stop") },
        // Nick after Long Time Coming (MS07cPostQuest, -Quests 0:18817F) stays unshared: MS07c's 305, a plain shared stage,
        // sets its 5 in every game, so a friend's game would run its timer and drive it, and the owner's 20 (COMNick 80:
        // SetCompanion) would recruit a second Nick there (SafeScan.SetsEach, StageRules.h SHARED_SETS_EACH).
        { Tuple.Create(0, 0x027411u, 5), Tuple.Create("plain", "MacCready out of Goodneighbor's crime faction, his intro scene: the world's, whoever recruits him (and never reported: no objective yet)") },
        { Tuple.Create(0, 0x027411u, 60), Tuple.Create("plain", "the factions are Barnes's and Winlock's, made Gunners: the world's") },
        { Tuple.Create(0, 0x16454Eu, 400), Tuple.Create("kEach", "its operation scene runs on the aliases 300 changed in the owner's game only") },
        { Tuple.Create(0, 0x16454Eu, 500), Tuple.Create("kEach", "the stimpak keyword on her new body (the owner's); the robot body's death is EACH's") },
        { Tuple.Create(0, 0x16454Eu, 620), Tuple.Create("kStory", "sets the Railroad's RR101 110 (kStory is decided by hand, as -Rules leaves it)") },
    };

    // The quests scanned: QuestFilter.h's COMPANION_QUESTS, every quest with a kEach row in StageRules.h's RULES (the
    // companions' stages of quests shared another way: Nick's, Cait's, Danse's, Dogmeat's), and any given by hand
    // (-Quests file:hex, for a look at a candidate before its rows exist).
    static List<Tuple<int, uint>> Quests(string a_questFilter, string a_stageRules, string[] a_extra)
    {
        var quests = new List<Tuple<int, uint>>();
        var text = File.ReadAllText(a_questFilter);
        int at = text.IndexOf("COMPANION_QUESTS[] = {", StringComparison.Ordinal);
        if (at < 0) throw new Exception("COMPANION_QUESTS not found in " + a_questFilter);
        var body = text.Substring(at, text.IndexOf("};", at, StringComparison.Ordinal) - at);
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(body, @"\{\s*(\d+),\s*0x([0-9A-Fa-f]+)\s*\}"))
            quests.Add(Tuple.Create(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16)));
        foreach (var row in SafeScan.Pays(a_stageRules, "kEach").OrderBy(r => r.Item1).ThenBy(r => r.Item2))
            if (!quests.Contains(Tuple.Create(row.Item1, row.Item2))) quests.Add(Tuple.Create(row.Item1, row.Item2));
        foreach (var extra in a_extra ?? new string[0])
        {
            var parts = extra.Split(':');
            var q = Tuple.Create(int.Parse(parts[0]), Convert.ToUInt32(parts[1], 16));
            if (!quests.Contains(q)) quests.Add(q);
        }
        return quests;
    }

    public static string Run(string data, string a_questFilter, string a_stageRules, string[] a_extra)
    {
        MoveScan.Load(data);
        var sb = new StringBuilder();
        var rows = new StringBuilder();
        foreach (var q in Quests(a_questFilter, a_stageRules, a_extra))
        {
            int file = q.Item1;
            uint local = q.Item2;
            GearScan.Quest quest;
            if (!MoveScan.perFile[file].TryGetValue(local, out quest)) { sb.AppendLine(string.Format("# {0} {1:X6}: no such quest", file, local)); continue; }
            sb.AppendLine(string.Format("# {0} {1:X6} {2} type {3}, flags {4:X4}, {5} objectives, start-up {6}, completes at {7}", file, local, quest.Edid, quest.Type, quest.Flags,
                quest.Objectives, string.Join(",", quest.Startup.Select(s => s.ToString()).ToArray()), string.Join(",", quest.Complete.Select(s => s.ToString()).ToArray())));
            // The aliases, for EACH rows and DRIVERS by hand: id, name, fill, and the reference a forced one holds.
            sb.AppendLine("#   aliases: " + string.Join(" ", quest.Aliases.Select(a => a.Id + ":" + a.Name + "(" + a.Fill + (a.Forced != 0 ? " " + a.Forced.ToString("X8") : "") + ")").ToArray()));
            var stages = MoveScan.Fragments(file, quest.Id);
            var calls = new Dictionary<int, List<string>>();
            var why = new Dictionary<int, List<string>>();
            foreach (var stage in stages.Keys)
            {
                calls[stage] = new List<string>();
                why[stage] = stages[stage].SelectMany(f => Owners(f, calls[stage])).Distinct().ToList();
            }
            // A stage that sets one of its own quest's stages that is the owner's (Curie's quickstart 5 sets 200-600): run
            // in a friend's game, it would run that stage's script there.
            foreach (var stage in stages.Keys)
                foreach (var f in stages[stage])
                    foreach (var i in f.Code)
                    {
                        int to;
                        if (i.Op == 23 && Is(i.Args[0], "SetStage") && Is(i.Args[1], "self") && i.Args.Count > 3 && int.TryParse(i.Args[3], out to) && to != stage && why.ContainsKey(to) && why[to].Any(w => !w.StartsWith("traits")))
                            why[stage].Add("sets its own stage " + to);
                    }
            bool first = true;
            foreach (var stage in stages.Keys.OrderBy(k => k))
            {
                var moves = MoveScan.StageMoves(file, quest, stage);
                var takes = stages[stage].SelectMany(f => QuestScan.Takes(f)).Where(t => t.Item2.Length > 0 && !t.Item2.StartsWith("temp", StringComparison.OrdinalIgnoreCase)).ToList();
                var wait = (int)Math.Min(255, Math.Round(10 * stages[stage].Sum(f => RuleScan.Waits(f))));
                var flags = new List<string>();
                if (moves != null) flags.Add("kMoves");
                if (takes.Count > 0) flags.Add("kPays");  // a payment is skipped whole: kEach adds nothing to it
                else if (why[stage].Any(w => !w.StartsWith("traits"))) flags.Add("kEach");
                else if (why[stage].Count > 0) flags.Add("kEach?");  // keywords, perks or factions only: whose, by hand
                var scanned = flags.Count > 0 ? string.Join(" | ", flags.ToArray()) : "plain";
                Tuple<string, string> hand;
                if (HAND.TryGetValue(Tuple.Create(file, local, stage), out hand))
                    flags = hand.Item1 == "plain" ? new List<string>() : new List<string>(flags.Where(x => x != "kEach?" && x != "kEach")) { hand.Item1 };
                flags = flags.Distinct().ToList();
                sb.AppendLine(string.Format("#   {0}: {1}{2}{3}{4}", stage, scanned, why[stage].Count > 0 ? " (" + string.Join("; ", why[stage].ToArray()) + ")" : "",
                    moves != null ? " moves: " + moves : "", hand != null ? " -> by hand " + hand.Item1 + ": " + hand.Item2 : ""));
                sb.AppendLine("#       " + string.Join(" ", calls[stage].ToArray()));
                if (flags.Count == 0) continue;
                if (flags.Contains("kEach")) { wait = 0; flags.Remove("kMoves"); }  // its script doesn't run in a friend's game: nothing moves the player there
                rows.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, {4} }},{5}", file, local, stage, string.Join(" | ", flags.ToArray()), wait, first ? "  // " + quest.Edid : ""));
                first = false;
            }
        }
        sb.Append(rows);
        return sb.ToString();
    }
}

// Which globals each stage of the shared quests reads (GetValue/GetValueInt on a global property), itself or through
// the functions it calls and the stages it sets or starts, walked as -Moves walks them: what src/game/StageRules.h's
// CARRIES was made from (the per-game globals among them, Nuka-World's gang ranks and parks, picked by hand; the rest,
// GameDaysPassed, a park's star-core count, are the game's own bookkeeping or the same in every game). With a_quests
// (a regular expression on the editor ID), those quests whether shared or not. One line per stage: file, form ID,
// editor ID, shared how, stage, the chain to the first read found (as -Moves's chains) and then every global the whole
// walk reads, each with the function that reads it (so a CARRIES row can be checked complete: MQ04 500's chain ends at
// the outpost tracker's count, and the ranks DLC04MakeEnemy reads behind it are in its every-read list).
public static class GlobalScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }

    // The globals a function's code reads, by name, or none.
    static List<string> Names(List<GearScan.Instr> code)
    {
        var names = new List<string>();
        foreach (var i in code)
            if (i.Op == 23 && (Is(i.Args[0], "GetValue") || Is(i.Args[0], "GetValueInt")) && !i.Args[1].StartsWith("temp", StringComparison.OrdinalIgnoreCase) && !i.Args[1].StartsWith("::temp", StringComparison.OrdinalIgnoreCase))
                names.Add(Bare(i.Args[1]));
        return names.Distinct().ToList();
    }

    static string Reads(List<GearScan.Instr> code)
    {
        var names = Names(code);
        return names.Count == 0 ? null : "reads " + string.Join(",", names.ToArray());
    }

    public static string Run(string a_quests)
    {
        var first = new MoveScan.Walk { Direct = Reads };
        // Every function the stage's walk reaches, and what each reads: Direct finds nothing, so the walk goes on past
        // each read into everything the stage calls, sets or starts.
        var byFunction = new SortedDictionary<string, SortedSet<string>>(StringComparer.OrdinalIgnoreCase);
        var every = new MoveScan.Walk
        {
            Direct = c => null,
            Visit = (f, code) =>
            {
                var names = Names(code);
                if (names.Count == 0) return;
                var key = f.Obj + "." + f.Name;
                SortedSet<string> set;
                if (!byFunction.TryGetValue(key, out set)) byFunction[key] = set = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var n in names) set.Add(n);
            }
        };
        var sb = new StringBuilder();
        sb.AppendLine("# file\tid\tedid\tshared\tstage\tfirst read (the chain to it)\tevery read (by the function that reads it)");
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var q in MoveScan.perFile[file].Values.OrderBy(x => x.Id & 0xFFFFFF))
            {
                var shared = QuestScan.Shared(q, file);
                if (!string.IsNullOrEmpty(a_quests) ? !System.Text.RegularExpressions.Regex.IsMatch(q.Edid, a_quests) : shared == "not shared") continue;
                foreach (var stage in MoveScan.Fragments(file, q.Id).Keys.OrderBy(k => k))
                {
                    var h = first.Stage(file, q, stage);
                    if (h == null) continue;
                    byFunction.Clear();
                    every.Stage(file, q, stage);
                    var all = string.Join("; ", byFunction.Select(kv => kv.Key + ": " + string.Join(",", kv.Value.ToArray())).ToArray());
                    sb.AppendLine(string.Format("{0}\t0x{1:X6}\t{2}\t{3}\t{4}\t{5}\t{6}", file, q.Id & 0xFFFFFF, q.Edid, shared, stage, h, all));
                }
            }
        return sb.ToString();
    }
}

// -Achievements: the shared quests' stages that award a Steam achievement (Game.AddAchievement) in their own fragment, and
// every award a stage reaches by stage sets alone (its fragment sets a stage, of its own quest or another, whose fragment
// awards, or sets one that does, and so on; MQ104 205 sets its 210; MQ201 150 sets MQ106 2000; BoSM04 1 sets BoS200 200
// and MQ206 1200, two rows): src/game/AchievementStages.h's rows. A stage that reaches an award only through a script
// function (a companion set, a settlement's happiness, the Minutemen's ally count) is left out and counted: that award is
// the game's own doing from its own state, not the stage's. With -Safe loaded, the SAFE and SILENT quests count as shared.
public static class AchievementScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }

    static string Awards(List<GearScan.Instr> code)
    {
        var ids = new List<string>();
        foreach (var i in code)
            if ((i.Op == 25 || i.Op == 23) && i.Args.Count > 3 && Is(i.Args[1], "AddAchievement")) ids.Add(i.Args[3]);
            else if (i.Op == 25 && i.Args.Count > 3 && Is(i.Args[0], "game") && Is(i.Args[1], "AddAchievement")) ids.Add(i.Args[3]);
        return ids.Count == 0 ? null : "awards " + string.Join(",", ids.Distinct().ToArray());
    }

    class Row { public int File; public GearScan.Quest Quest; public int Stage; public int AwardFile; public GearScan.Quest Award; public int AwardStage; public string Ids; }

    public static string Run(string data)
    {
        MoveScan.Load(data);
        var walk = new MoveScan.Walk { Direct = Awards };  // through script functions too: what a stage with no row reaches that way
        var rows = new List<Row>();
        var left = new List<string>();
        var awards = new Dictionary<Tuple<int, uint, int>, string>();  // what a stage's own fragments award, by (file, quest, stage)
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var q in MoveScan.perFile[file].Values.OrderBy(x => x.Id & 0xFFFFFF))
            {
                if (QuestScan.Shared(q, file) == "not shared") continue;
                foreach (var stage in MoveScan.Fragments(file, q.Id).Keys.OrderBy(k => k))
                {
                    // every stage reached by stage sets alone, the stage itself first, each once
                    var seen = new HashSet<Tuple<int, uint, int>>();
                    var todo = new Queue<Tuple<int, GearScan.Quest, int>>();
                    todo.Enqueue(Tuple.Create(file, q, stage));
                    seen.Add(Tuple.Create(file, q.Id & 0xFFFFFF, stage));
                    int before = rows.Count;
                    while (todo.Count > 0)
                    {
                        var at = todo.Dequeue();
                        var key = Tuple.Create(at.Item1, at.Item2.Id & 0xFFFFFF, at.Item3);
                        string ids;
                        if (!awards.TryGetValue(key, out ids)) awards[key] = ids = Awards(MoveScan.FragmentCode(at.Item1, at.Item2.Id, at.Item3));
                        if (ids != null) rows.Add(new Row { File = file, Quest = q, Stage = stage, AwardFile = at.Item1, Award = at.Item2, AwardStage = at.Item3, Ids = ids.Substring("awards ".Length) });
                        foreach (var next in MoveScan.StageSets(at.Item1, at.Item2, at.Item3))
                            if (seen.Add(Tuple.Create(next.Item1, next.Item2.Id & 0xFFFFFF, next.Item3))) todo.Enqueue(next);
                    }
                    if (rows.Count > before) continue;
                    var chain = walk.Stage(file, q, stage);
                    if (chain != null) left.Add(string.Format("# {0} 0x{1:X6} {2} stage {3}: {4}", file, q.Id & 0xFFFFFF, q.Edid, stage, chain));
                }
            }
        rows.Sort((a, b) =>
        {
            int c = a.File.CompareTo(b.File); if (c != 0) return c;
            c = (a.Quest.Id & 0xFFFFFF).CompareTo(b.Quest.Id & 0xFFFFFF); if (c != 0) return c;
            c = a.Stage.CompareTo(b.Stage); if (c != 0) return c;
            c = a.AwardFile.CompareTo(b.AwardFile); if (c != 0) return c;
            c = (a.Award.Id & 0xFFFFFF).CompareTo(b.Award.Id & 0xFFFFFF); if (c != 0) return c;
            return a.AwardStage.CompareTo(b.AwardStage);
        });
        var sb = new StringBuilder();
        sb.AppendLine(string.Format("# {0} rows (stage sets alone; {1} stages); {2} stage(s) reach an award only through a script function, left out:", rows.Count,
            rows.Select(r => Tuple.Create(r.File, r.Quest.Id & 0xFFFFFF, r.Stage)).Distinct().Count(), left.Count));
        foreach (var l in left) sb.AppendLine(l);
        foreach (var r in rows)
            sb.AppendLine(string.Format("\t\t{{ {0}, 0x{1:X6}, {2}, {3}, 0x{4:X6}, {5} }},  // {6} {7}{8}: {9}", r.File, r.Quest.Id & 0xFFFFFF, r.Stage, r.AwardFile, r.Award.Id & 0xFFFFFF, r.AwardStage,
                r.Quest.Edid, r.Stage, r.Award == r.Quest && r.AwardStage == r.Stage ? "" : " -> " + r.Award.Edid + " " + r.AwardStage, r.Ids));
        return sb.ToString();
    }
}

// -StoryEvents: the story manager's event handlers (OnStoryScript, OnStoryKillActor and the other OnStory* events of a
// quest's script) of the quests a friend's game starts with no event, and what each does. A friend's game starts the
// host's run of a radiant (src/game/RadiantRules.h), the host's settlement attack as its mirror (common/AttackBook.h's
// ATTACKS) and a random encounter shared by its run (src/game/EncounterRunBook.h: a misc quest an event starts, named
// RE..., with objectives) through QuestStart::Start, which calls TESQuest::EnsureQuestStarted with no BGSStoryEvent: the
// quest's scripts get no OnStory* event there, so what a handler sets (from the event's values or at all) is never set
// in that game. Listed too: every other quest a story manager event starts (ENAM) that a friend's stage reaches
// (QuestScan.ReachesFriend; a friend's stage would start it from that stage, not from the event), and any quest named
// by -Quests <regex on its editor ID>. Per handler (once, with the quests whose scripts have it): each parameter it
// reads and where the value goes (a script variable or property, an alias, a global, a stage, a call, a branch),
// followed into the script functions it is passed to; what the handler and the functions it calls on its own script set
// and call (calls named Get*, Is* or Has* on another object left out: they read); and the other functions that read
// what it sets (of its script, the scripts it extends or that extend it, and a property read on a variable of such a
// script: a fragment's kmyQuest). Then the quests with no handler, by group, and the attached scripts not found in the
// masters' archives.
public static class StoryEventScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }
    static bool Temp(string a) { return a.StartsWith("::temp", StringComparison.OrdinalIgnoreCase) || Is(a, "::nonevar"); }
    static bool Reads(string a_name) { return a_name.StartsWith("Get", StringComparison.OrdinalIgnoreCase) || a_name.StartsWith("Is", StringComparison.OrdinalIgnoreCase) || a_name.StartsWith("Has", StringComparison.OrdinalIgnoreCase); }

    // A script's variable or property (its auto property's ::Name_var): not a literal, nor a parameter, local or temp of the function.
    static bool ScriptVar(GearScan.Func f, string a)
    {
        if (string.IsNullOrEmpty(a) || Temp(a) || Is(a, "self") || Is(a, "None") || a.StartsWith("\"")) return false;
        double n;
        if (a == "True" || a == "False" || double.TryParse(a, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out n)) return false;
        return !f.Types.ContainsKey(a);
    }

    // Whether script a_obj is a_script or extends it.
    static bool Extends(string a_obj, string a_script)
    {
        for (int depth = 0; !string.IsNullOrEmpty(a_obj) && depth < 16; depth++)
        {
            if (Is(a_obj, a_script)) return true;
            a_obj = MoveScan.ParentOf(a_obj);
        }
        return false;
    }

    static bool Related(string a_obj, string a_script) { return Extends(a_obj, a_script) || Extends(a_script, a_obj); }

    // A call's receiver as a reader knows it: self, the variable's name, or a temp's type.
    static string Receiver(GearScan.Func f, string a_recv)
    {
        if (Is(a_recv, "self")) return "self";
        if (!Temp(a_recv)) return Bare(a_recv);
        var t = MoveScan.TypeOf(f, a_recv);
        return t != null ? "(" + t + ")" : "?";
    }

    class Call { public string Name; public string Recv; public string Dest; public List<string> Args; public GearScan.Func Callee; }

    // A call instruction (callmethod, callparent, callstatic): its function, receiver, result and arguments, and the script
    // function it runs when it is one of the masters' (null: a native one, or a receiver of no known script).
    static Call CallOf(GearScan.Func f, GearScan.Instr i)
    {
        var c = new Call();
        string type;
        if (i.Op == 23 && i.Args.Count > 2) { c.Name = i.Args[0]; c.Recv = i.Args[1]; c.Dest = i.Args[2]; c.Args = i.Args.Skip(3).ToList(); type = MoveScan.TypeOf(f, c.Recv); }
        else if (i.Op == 24 && i.Args.Count > 1) { c.Name = i.Args[0]; c.Recv = "parent"; c.Dest = i.Args[1]; c.Args = i.Args.Skip(2).ToList(); type = MoveScan.ParentOf(f.Obj); }
        else if (i.Op == 25 && i.Args.Count > 2) { c.Name = i.Args[1]; c.Recv = i.Args[0]; c.Dest = i.Args[2]; c.Args = i.Args.Skip(3).ToList(); type = i.Args[0]; }
        else return null;
        string owner;
        var g = string.IsNullOrEmpty(type) ? null : MoveScan.Find(type, c.Name, out owner);
        c.Callee = g != null && g.Code.Count > 0 ? g : null;
        return c;
    }

    // The arguments an instruction (not a call: CallOf) reads, and the one it writes (null: none).
    static void Operands(GearScan.Instr i, out List<string> a_reads, out string a_dest)
    {
        var a = i.Args;
        a_reads = new List<string>(); a_dest = null;
        switch (i.Op)
        {
            case 0: case 20: return;
            case 21: case 22: case 26: if (a.Count > 0) a_reads.Add(a[0]); return;
            case 28: if (a.Count > 2) { a_reads.Add(a[1]); a_dest = a[2]; } return;  // propget name, obj, dest
            case 29: case 33: case 39: a_reads.AddRange(a.Skip(1)); if (a.Count > 0) a_dest = a[0]; return;  // propset name, obj, value; array_setelement arr, index, value; struct_set
            case 34: case 35: case 40: case 41: if (a.Count > 1) { a_reads.Add(a[0]); a_reads.AddRange(a.Skip(2)); a_dest = a[1]; } return;  // array_find*: arr, dest, ...
            case 42: case 43: case 44: case 45: case 46: a_reads.AddRange(a); if (a.Count > 0) a_dest = a[0]; return;  // array_add arr, value ...: the array changes
            default: if (a.Count > 0) { a_dest = a[0]; a_reads.AddRange(a.Skip(1)); } return;
        }
    }

    // What the instructions a branch skips (the jmpt or jmpf at a_at) set and call: "sets X, calls Y.Z", or "".
    static string Guarded(GearScan.Func f, int a_at)
    {
        int skip;
        if (f.Code[a_at].Args.Count < 2 || !int.TryParse(f.Code[a_at].Args[1], out skip) || skip <= 1) return "";
        var did = new List<string>();
        for (int k = a_at + 1; k < f.Code.Count && k < a_at + skip; k++)
        {
            var i = f.Code[k];
            if (i.Op >= 23 && i.Op <= 25)
            {
                var c = CallOf(f, i);
                if (c != null && !Reads(c.Name)) did.Add("calls " + (i.Op == 25 ? c.Recv : Receiver(f, c.Recv)) + "." + c.Name);
                if (c != null && c.Dest != null && ScriptVar(f, c.Dest)) did.Add("sets " + Bare(c.Dest));
                continue;
            }
            List<string> reads; string dest;
            Operands(i, out reads, out dest);
            if (i.Op == 26) did.Add("returns");
            else if (i.Op == 29) did.Add("sets property " + Receiver(f, i.Args[1]) + "." + i.Args[0]);
            else if (dest != null && i.Op != 21 && i.Op != 22 && ScriptVar(f, dest)) did.Add("sets " + Bare(dest));
        }
        return did.Count == 0 ? "" : " (" + string.Join(", ", did.Distinct().ToArray()) + ")";
    }

    // Where the values held in a_tainted (parameters of f) go in f: a_out gets one line per use, a_via in front of those
    // found in a function the handler passed them to (to depth 3). Read in the code's order, a temp written from other
    // values no longer holds it (the compiler reuses its temps); a named variable keeps it, whatever path wrote it.
    static void Flow(GearScan.Func f, HashSet<string> a_tainted, string a_via, int a_depth, SortedSet<string> a_out, HashSet<string> a_seen)
    {
        var key = f.Obj + "." + f.State + "." + f.Name + "(" + string.Join(",", a_tainted.OrderBy(x => x).ToArray()) + ")";
        if (!a_seen.Add(key)) return;
        var t = new HashSet<string>(a_tainted, StringComparer.OrdinalIgnoreCase);
        Func<string, bool> has = v => v != null && t.Contains(v);
        for (int pass = 0; pass < 2; pass++)  // twice: a loop's read of a later write (the temps start clean)
        {
            t.RemoveWhere(Temp);
            for (int at = 0; at < f.Code.Count; at++)
            {
                var i = f.Code[at];
                if (i.Op >= 23 && i.Op <= 25)
                {
                    var c = CallOf(f, i);
                    if (c == null) continue;
                    bool onIt = i.Op == 23 && has(c.Recv);
                    var passed = c.Args.Select((x, k) => has(x) ? k : -1).Where(k => k >= 0).ToList();
                    if (!onIt && passed.Count == 0) { if (c.Dest != null && Temp(c.Dest)) t.Remove(c.Dest); continue; }
                    if (c.Dest != null && ScriptVar(f, c.Dest)) a_out.Add(a_via + "sets " + Bare(c.Dest) + " (" + c.Name + "'s result)");
                    if (c.Dest != null && !Is(c.Dest, "::nonevar")) t.Add(c.Dest);
                    var recv = i.Op == 25 ? c.Recv : Receiver(f, c.Recv);
                    if (onIt) a_out.Add(a_via + "calls " + c.Name + " on it");
                    else if (Is(c.Name, "ForceRefTo") || Is(c.Name, "ForceLocationTo") || Is(c.Name, "AddRef")) a_out.Add(a_via + "fills alias " + recv);
                    else if (Is(c.Name, "SetValue") || Is(c.Name, "SetValueInt") || Is(c.Name, "Mod")) a_out.Add(a_via + "sets global " + recv);
                    else if (Is(c.Name, "SetStage")) a_out.Add(a_via + "is the stage set on " + recv);
                    else a_out.Add(a_via + "passed to " + recv + "." + c.Name);
                    if (c.Callee != null && a_depth < 3 && passed.Count > 0)
                    {
                        var mapped = new HashSet<string>(passed.Where(k => k < c.Callee.Params.Count).Select(k => c.Callee.Params[k]), StringComparer.OrdinalIgnoreCase);
                        if (mapped.Count > 0) Flow(c.Callee, mapped, a_via + c.Callee.Obj + "." + c.Callee.Name + ": ", a_depth + 1, a_out, a_seen);
                    }
                    continue;
                }
                List<string> reads; string dest;
                Operands(i, out reads, out dest);
                if (!reads.Any(has)) { if (dest != null && Temp(dest) && !(i.Op == 29 || i.Op == 33 || i.Op == 39 || i.Op >= 42)) t.Remove(dest); continue; }  // not a write in place
                if (i.Op == 21 || i.Op == 22) { a_out.Add(a_via + "decides a branch" + Guarded(f, at)); continue; }
                if (i.Op == 26) { a_out.Add(a_via + "is returned"); continue; }
                if (dest == null) continue;
                if (i.Op == 29) { a_out.Add(a_via + "sets property " + Receiver(f, i.Args[1]) + "." + i.Args[0]); continue; }
                if (ScriptVar(f, dest)) a_out.Add(a_via + "sets " + Bare(dest));
                t.Add(dest);
            }
        }
    }

    // What a function and the functions it calls on its own script (to depth 2) set (script variables and properties;
    // a_written keeps the pex names of its own script's) and call: lines, each with the function it is in when not the handler.
    static void Effects(GearScan.Func f, string a_via, int a_depth, List<string> a_sets, List<string> a_calls, HashSet<string> a_written, HashSet<GearScan.Func> a_visited)
    {
        if (!a_visited.Add(f)) return;
        foreach (var i in f.Code)
        {
            if (i.Op >= 23 && i.Op <= 25)
            {
                var c = CallOf(f, i);
                if (c == null) continue;
                if (c.Dest != null && ScriptVar(f, c.Dest)) { a_written.Add(c.Dest); a_sets.Add(a_via + Bare(c.Dest) + " (" + c.Name + "'s result)"); }
                if (Reads(c.Name)) continue;
                bool own = i.Op == 24 || (i.Op == 23 && (Is(c.Recv, "self") || Is(c.Recv, "kmyQuest")));
                var shown = c.Args.Select(x => x.StartsWith("::temp", StringComparison.OrdinalIgnoreCase) ? "_" : Bare(x)).ToArray();
                a_calls.Add(a_via + (i.Op == 25 ? c.Recv : Receiver(f, c.Recv)) + "." + c.Name + "(" + string.Join(", ", shown) + ")");
                // into the script's own functions, not the game's base scripts' (Quest.SetStage)
                if (own && c.Callee != null && a_depth < 2 && !Is(c.Callee.Obj, "Quest") && !Is(c.Callee.Obj, "ScriptObject"))
                    Effects(c.Callee, a_via + c.Callee.Name + ": ", a_depth + 1, a_sets, a_calls, a_written, a_visited);
                continue;
            }
            List<string> reads; string dest;
            Operands(i, out reads, out dest);
            if (dest == null || i.Op == 21 || i.Op == 22 || i.Op == 26) continue;
            if (i.Op == 29) { if (Is(i.Args[1], "self")) a_written.Add(i.Args[0]); a_sets.Add(a_via + "property " + Receiver(f, i.Args[1]) + "." + i.Args[0]); continue; }
            if (ScriptVar(f, dest)) { a_written.Add(dest); a_sets.Add(a_via + Bare(dest)); }
        }
    }

    // The functions but a_skip's that read a script variable a_var of a_script (or of a script it extends or that extends
    // it), or read it as a property through a variable of such a script (a fragment's kmyQuest.attackStrength).
    static List<string> Readers(string a_script, string a_var, HashSet<GearScan.Func> a_skip)
    {
        var found = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        var bare = Bare(a_var);
        foreach (var g in MoveScan.AllFunctions())
        {
            if (a_skip.Contains(g)) continue;
            bool related = Related(g.Obj, a_script);
            foreach (var i in g.Code)
            {
                if (i.Op == 28 && i.Args.Count > 1 && Is(i.Args[0], bare))
                {
                    var t = Is(i.Args[1], "self") ? g.Obj : MoveScan.TypeOf(g, i.Args[1]);
                    if (t != null && Related(t, a_script)) { found.Add(g.Obj + "." + g.Name); break; }
                }
                if (!related || g.Types.ContainsKey(a_var)) continue;
                List<string> reads; string dest;
                if (i.Op >= 23 && i.Op <= 25) { var c = CallOf(g, i); reads = c == null ? new List<string>() : c.Args.Concat(new[] { c.Recv }).ToList(); }
                else Operands(i, out reads, out dest);
                if (reads.Any(r => Is(r, a_var))) { found.Add(g.Obj + "." + g.Name); break; }
            }
        }
        return found.ToList();
    }

    // Readers as a line: a quest's stage fragments as one entry with their stages (QF_MinRecruit01_00098136 stages 20, 80).
    static string Summary(List<string> a_readers)
    {
        var parts = new List<string>();
        var stages = new SortedDictionary<string, SortedSet<int>>(StringComparer.OrdinalIgnoreCase);
        foreach (var r in a_readers)
        {
            var m = System.Text.RegularExpressions.Regex.Match(r, @"^Fragments:Quests:(\w+)\.Fragment_Stage_(\d+)_");
            if (!m.Success) { parts.Add(r); continue; }
            if (!stages.ContainsKey(m.Groups[1].Value)) stages[m.Groups[1].Value] = new SortedSet<int>();
            stages[m.Groups[1].Value].Add(int.Parse(m.Groups[2].Value));
        }
        parts.AddRange(stages.Select(kv => kv.Key + " stage" + (kv.Value.Count > 1 ? "s " : " ") + string.Join(" ", kv.Value.Select(s => s.ToString()).ToArray())));
        return parts.Count == 0 ? "by nothing else" : string.Join(", ", parts.ToArray());
    }

    class Scoped { public int File; public GearScan.Quest Quest; public string Group; }

    // The quests in scope, each once, under the first group that names it: RadiantRules.h's, the attacks, the encounters
    // shared by their run, the other event quests a friend's stage reaches, -Quests'.
    static List<Scoped> Scope(string a_radiantRules, string a_attackBook, string a_quests)
    {
        var list = new List<Scoped>();
        var seen = new HashSet<Tuple<int, uint>>();
        Action<int, uint, string> add = (file, id, group) =>
        {
            GearScan.Quest q;
            if (file < 0 || file >= MoveScan.perFile.Count || !MoveScan.perFile[file].TryGetValue(id & 0xFFFFFF, out q)) { list.Add(new Scoped { File = file, Group = group + " " + file + ":" + id.ToString("X6") + " is not in the masters" }); return; }
            if (seen.Add(Tuple.Create(file, id & 0xFFFFFF))) list.Add(new Scoped { File = file, Quest = q, Group = group });
        };
        int radiants = 0;
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(a_radiantRules), "Entry\\{\\s*(\\d),\\s*0x([0-9A-Fa-f]+),\\s*\"(\\w+)\",\\s*Share::(k\\w+)([^}]*)\\}"))
        {
            var flags = m.Groups[5].Value.Split(',').Select(x => x.Trim()).Where(x => x.Length > 0).ToList();
            add(int.Parse(m.Groups[1].Value), Convert.ToUInt32(m.Groups[2].Value, 16), "radiant " + m.Groups[4].Value + (flags.Count > 2 && flags[2] == "true" ? " attack" : ""));
            radiants++;
        }
        if (radiants == 0) throw new Exception("no RADIANTS row matched Entry{ file, 0xID, \"EditorID\", Share::k... } in " + a_radiantRules + ": the row format changed?");
        var book = File.ReadAllText(a_attackBook);
        var esms = new Dictionary<string, int>();
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(book, "constexpr std::string_view (\\w+) = \"([^\"]+)\";"))
            esms[m.Groups[1].Value] = Array.FindIndex(GearScan.MASTERS, x => Is(x.Esm, m.Groups[2].Value));
        int attacks = 0;
        foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(book, "\\{\\s*(\\w+),\\s*0x([0-9A-Fa-f]+),\\s*\"(\\w+)\"\\s*\\}"))
        {
            int file;
            if (!esms.TryGetValue(m.Groups[1].Value, out file)) continue;
            add(file, Convert.ToUInt32(m.Groups[2].Value, 16), "attack");
            attacks++;
        }
        if (attacks == 0) throw new Exception("no ATTACKS row matched { FILE, 0xID, \"EditorID\" } in " + a_attackBook + ": the row format changed?");
        for (int file = 0; file < MoveScan.perFile.Count; file++)
            foreach (var q in MoveScan.perFile[file].Values.OrderBy(x => x.Id & 0xFFFFFF))
            {
                if (q.Event.Length == 0) continue;
                // Encounters::IsEncounter and EncounterRunBook's SharesRun: misc, an event, RE and a capital or _ after the add-on's prefix, objectives
                var name = System.Text.RegularExpressions.Regex.Replace(q.Edid, "^DLC\\d+_?", "");
                bool encounter = q.Type == 6 && q.Objectives > 0 && name.Length > 2 && name.StartsWith("RE", StringComparison.Ordinal) && (name[2] == '_' || char.IsUpper(name[2]));
                if (encounter) add(file, q.Id, "encounter");
                else if (QuestScan.ReachesFriend(q, file)) add(file, q.Id, "event, " + QuestScan.Shared(q, file));
            }
        if (!string.IsNullOrEmpty(a_quests))
            for (int file = 0; file < MoveScan.perFile.Count; file++)
                foreach (var q in MoveScan.perFile[file].Values.OrderBy(x => x.Id & 0xFFFFFF))
                    if (System.Text.RegularExpressions.Regex.IsMatch(q.Edid, a_quests)) add(file, q.Id, "-Quests");
        return list;
    }

    static string Kind(string a_group) { return a_group.Split(' ', ',')[0]; }

    public static string Run(string a_radiantRules, string a_attackBook, string a_quests)
    {
        var scope = Scope(a_radiantRules, a_attackBook, a_quests);
        // each handler (a script's own function, in any state) with the quests whose scripts have it
        var handlers = new Dictionary<GearScan.Func, List<Scoped>>();
        var none = new SortedDictionary<string, int>();
        var missing = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        var sb = new StringBuilder();
        foreach (var s in scope.Where(x => x.Quest == null)) sb.AppendLine("# " + s.Group);
        foreach (var s in scope.Where(x => x.Quest != null))
        {
            bool any = false;
            foreach (var script in s.Quest.Scripts.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                var chain = MoveScan.Functions(script);
                if (chain == null) { missing.Add(script + " (" + s.Quest.Edid + ")"); continue; }
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var f in chain)
                {
                    if (!f.Name.StartsWith("OnStory", StringComparison.OrdinalIgnoreCase)) continue;
                    if (!names.Add(f.State + "." + f.Name)) continue;  // overridden by the script that extends it
                    if (f.Code.Count == 0) continue;  // Quest's own, empty
                    any = true;
                    if (!handlers.ContainsKey(f)) handlers[f] = new List<Scoped>();
                    if (!handlers[f].Contains(s)) handlers[f].Add(s);
                }
            }
            if (!any) { var k = Kind(s.Group); none[k] = none.ContainsKey(k) ? none[k] + 1 : 1; }
        }
        var counts = scope.Where(x => x.Quest != null).GroupBy(x => Kind(x.Group)).Select(g => g.Count() + " " + g.Key).ToArray();
        sb.AppendLine(string.Format("# {0} quests a friend's game starts with no story event or a friend's stage reaches: {1}", scope.Count(x => x.Quest != null), string.Join(", ", counts)));
        sb.AppendLine(string.Format("# {0} OnStory* handler(s) in their scripts, for {1} of them", handlers.Count, handlers.Values.SelectMany(x => x).Distinct().Count()));
        foreach (var f in handlers.Keys.OrderBy(x => x.Obj, StringComparer.OrdinalIgnoreCase).ThenBy(x => x.Name, StringComparer.OrdinalIgnoreCase))
        {
            var quests = handlers[f];
            sb.AppendLine();
            sb.AppendLine(string.Format("== {0}.{1}({2}){3}: {4} quest(s)", f.Obj, f.Name, string.Join(", ", f.Params.ToArray()), f.State.Length > 0 ? " in state " + f.State : "", quests.Count));
            foreach (var p in f.Params)
            {
                var uses = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
                Flow(f, new HashSet<string>(new[] { p }, StringComparer.OrdinalIgnoreCase), "", 0, uses, new HashSet<string>());
                sb.AppendLine("  " + p + ": " + (uses.Count == 0 ? "not read" : string.Join("; ", uses.ToArray())));
            }
            var sets = new List<string>(); var calls = new List<string>(); var written = new HashSet<string>(StringComparer.OrdinalIgnoreCase); var visited = new HashSet<GearScan.Func>();
            Effects(f, "", 0, sets, calls, written, visited);
            sb.AppendLine("  sets: " + (sets.Count == 0 ? "nothing" : string.Join("; ", sets.Distinct().ToArray())));
            sb.AppendLine("  calls: " + (calls.Count == 0 ? "nothing" : string.Join("; ", calls.Distinct().ToArray())));
            foreach (var v in written.OrderBy(x => Bare(x), StringComparer.OrdinalIgnoreCase))
            {
                sb.AppendLine("  read: " + Bare(v) + ": " + Summary(Readers(f.Obj, v, visited)));
            }
            foreach (var s in quests.OrderBy(x => x.File).ThenBy(x => x.Quest.Id & 0xFFFFFF))
                sb.AppendLine(string.Format("  quest {0} 0x{1:X6} {2} ({3}; event {4})", s.File, s.Quest.Id & 0xFFFFFF, s.Quest.Edid, s.Group, s.Quest.Event.Length > 0 ? s.Quest.Event : "none"));
        }
        sb.AppendLine();
        sb.AppendLine("# no handler: " + (none.Count == 0 ? "none" : string.Join(", ", none.Select(kv => kv.Value + " " + kv.Key).ToArray())));
        sb.AppendLine("# scripts not in the masters' archives: " + (missing.Count == 0 ? "none" : string.Join(", ", missing.ToArray())));
        return sb.ToString();
    }
}
