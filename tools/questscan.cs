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
    public class Quest { public uint Id; public string Edid = ""; public int Type = -1; public int Flags; public string Full = ""; public int Stages; public int Objectives; }

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
                            if (st == "INDX") q.Stages++;
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
    public class Func { public string Obj; public string State; public string Name; public List<Instr> Code = new List<Instr>(); }

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
        int pc = r.U16(); for (int i = 0; i < pc; i++) { r.U16(); r.U16(); }
        int lc = r.U16(); for (int i = 0; i < lc; i++) { r.U16(); r.U16(); }
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

    public static List<Func> Pex(byte[] b)
    {
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
            r.U16(); r.U16(); r.U8(); r.U32(); r.U16();
            int structs = r.U16();
            for (int s = 0; s < structs; s++) { r.U16(); int m = r.U16(); for (int k = 0; k < m; k++) { r.U16(); r.U16(); r.U32(); r.Var(); r.U8(); r.U16(); } }
            int vc = r.U16(); for (int v = 0; v < vc; v++) { r.U16(); r.U16(); r.U32(); r.Var(); r.U8(); }
            int pcnt = r.U16();
            for (int p = 0; p < pcnt; p++)
            {
                string pn = r.Str(); r.U16(); r.U16(); r.U32(); byte fl = r.U8();
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

    static string Shared(GearScan.Quest q, int a_file)
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
