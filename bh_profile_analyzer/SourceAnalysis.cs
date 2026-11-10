using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.Diagnostics.Tracing.Etlx;

namespace BhProfileAnalyzer;

internal sealed record SourceSite(string Function, string? File, uint? Line, uint? Column, uint SymbolId);
internal sealed record SourceMapping(string Status, string InlineStatus, SourceSite? Location, SourceSite[] Contexts);
internal sealed class SourceAnalysis : IDisposable
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct NativeFrame
    {
        public uint Id, Parent, Line, Column, Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)] public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 1024)] public string File;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct NativeResult
    {
        public uint Size, Count, Flags;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public NativeFrame[] Frames;
    }
    [DllImport("bh_profile_dia.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    static extern int bh_dia_open(string dll, string pdb, Guid guid, uint age, out IntPtr context);
    [DllImport("bh_profile_dia.dll", CallingConvention = CallingConvention.Cdecl)]
    static extern int bh_dia_resolve(IntPtr context, uint rva, ref NativeResult result);
    [DllImport("bh_profile_dia.dll", CallingConvention = CallingConvention.Cdecl)]
    static extern void bh_dia_close(IntPtr context);

    sealed record AddressKey(string FunctionId, int Module, ulong Address, uint? Rva, string? Failure);
    readonly Dictionary<AddressKey, long> samples = new();
    readonly Dictionary<int, TraceModuleFile> modules = new();
    readonly Dictionary<int, (IntPtr Handle, string? Error)> sessions = new();
    readonly TraceLoadedModule[] loads;
    readonly string? symbols;
    public SourceAnalysis(TraceProcess process, string? symbols) { loads = process.LoadedModules.ToArray(); this.symbols = symbols; }
    public void Add(TraceCodeAddress? address, Frame frame, double time)
    {
        uint? rva = null;
        string? failure = null;
        var module = address?.ModuleFile;
        int index = module == null ? -1 : (int)module.ModuleFileIndex;
        if (address == null) failure = "missing_stack";
        else if (module == null) failure = "unknown_module";
        else
        {
            modules.TryAdd(index, module);
            var matches = loads.Where(m => m.ModuleFile.ModuleFileIndex == module.ModuleFileIndex &&
                m.LoadTimeRelativeMSec <= time && time < m.UnloadTimeRelativeMSec &&
                address.Address >= m.ImageBase && address.Address - m.ImageBase < (ulong)module.ImageSize).Take(2).ToArray();
            if (matches.Length != 1 || address.Address - matches[0].ImageBase > uint.MaxValue) failure = "module_lifetime_ambiguous_or_missing";
            else rva = (uint)(address.Address - matches[0].ImageBase);
        }
        var key = new AddressKey(frame.Id, index, address?.Address ?? 0, rva, failure);
        if (samples.Count >= 200000 && !samples.ContainsKey(key)) throw new InvalidDataException("source address bound exceeded");
        samples.TryGetValue(key, out var count); samples[key] = count + 1;
    }
    (IntPtr Handle, string? Error) Open(int index)
    {
        if (sessions.TryGetValue(index, out var existing)) return existing;
        IntPtr handle = IntPtr.Zero;
        string? failure = null;
        try
        {
            var module = modules[index];
            if (symbols == null) failure = "symbols_not_requested";
            else if (module.PdbSignature == Guid.Empty || string.IsNullOrEmpty(module.PdbName)) failure = "pdb_identity_missing";
            else
            {
                // Only an explicitly allowed directory, not embedded build paths or
                // symbol servers. Native DIA additionally validates GUID AND age.
                string pdb = Path.Combine(symbols, Path.GetFileName(module.PdbName));
                if (!File.Exists(pdb)) failure = "matching_local_pdb_unavailable";
                else
                {
                    int hr = bh_dia_open(Path.Combine(AppContext.BaseDirectory, "amd64", "msdia140.dll"), pdb,
                        module.PdbSignature, (uint)module.PdbAge, out handle);
                    if (hr < 0 || handle == IntPtr.Zero) failure = "pdb_validation_or_dia_open_failed:0x" + hr.ToString("x8");
                }
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or IOException or UnauthorizedAccessException)
        { failure = "dia_helper_unavailable:" + ex.GetType().Name; }
        return sessions[index] = (handle, failure);
    }
    internal static SourceMapping Normalize(NativeResult result)
    {
        if (result.Count == 0 || result.Count > 17) return new("source_unavailable", "unavailable", null, []);
        static SourceSite Site(NativeFrame f) => new(f.Name, f.Line > 0 && f.Flags == 0 ? f.File : null,
            f.Line > 0 && f.Flags == 0 ? f.Line : null, f.Line > 0 && f.Flags == 0 ? f.Column : null, f.Id);
        var physical = result.Frames[0];
        var chain = new List<SourceSite> { Site(physical) };
        if (result.Flags != 0) return new("inline_mapping_incomplete", (result.Flags & 2) != 0 ? "truncated" : "unavailable", null, chain.ToArray());
        var remaining = result.Frames.Skip(1).Take((int)result.Count - 1).ToList();
        uint parent = physical.Id;
        var seen = new HashSet<uint> { parent };
        while (remaining.Count > 0)
        {
            var next = remaining.Where(f => f.Parent == parent).ToArray();
            if (next.Length != 1 || !seen.Add(next[0].Id)) return new("inline_chain_ambiguous", "ambiguous", null, chain.ToArray());
            var frame = next[0];
            chain.Add(Site(frame)); parent = frame.Id;
            remaining.Remove(frame); // retain duplicates so an ambiguous PDB cannot masquerade as a chain
        }
        var leaf = chain[^1];
        return new(leaf.Line == null ? "source_unavailable" : "resolved", chain.Count > 1 ? "available" : "none_reported", leaf, chain.ToArray());
    }
    SourceMapping Resolve(AddressKey key)
    {
        if (key.Failure != null) return new(key.Failure, "unavailable", null, []);
        var session = Open(key.Module);
        if (session.Error != null) return new(session.Error, "unavailable", null, []);
        var result = new NativeResult { Size = (uint)Marshal.SizeOf<NativeResult>(), Frames = new NativeFrame[17] };
        int hr = bh_dia_resolve(session.Handle, key.Rva!.Value, ref result);
        if (hr != 0) return new("dia_resolve_unavailable:0x" + hr.ToString("x8"), "unavailable", null, []);
        return Normalize(result);
    }
    public object Report(string selector, Aggregation all, int top, long processSamples, long processMissingStacks, long processUnresolvedLeafSamples)
    {
        var selected = all.Frames.Values.Where(f => f.Id == selector || (f.Resolved && f.Name == selector)).ToArray();
        if (selected.Length > 1) throw new InvalidDataException("ambiguous function name; use exact function id from analysis");
        var function = selected.SingleOrDefault();
        var scoped = samples.Where(s => s.Key.FunctionId == function?.Id).ToArray();
        long scope = scoped.Sum(s => s.Value);
        var rows = new Dictionary<string, (SourceMapping Mapping, long Count, HashSet<ulong> Addresses)>();
        foreach (var entry in scoped)
        {
            var mapping = Resolve(entry.Key);
            // Call-site context is part of the key; identical inline bodies at
            // distinct callers must not be silently merged.
            string key = JsonSerializer.Serialize(mapping);
            if (!rows.TryGetValue(key, out var row)) row = (mapping, 0, new());
            row.Count += entry.Value; row.Addresses.Add(entry.Key.Address); rows[key] = row;
        }
        var sorted = rows.OrderByDescending(r => r.Value.Count).ThenBy(r => r.Key, StringComparer.Ordinal).ToArray();
        long resolved = rows.Values.Where(r => r.Mapping.Status == "resolved").Sum(r => r.Count);
        return new {
            selector, status = all.Samples == 0 ? "no_retained_samples" : function == null ? "function_unresolved_or_absent" : scope == 0 ? "no_self_samples" : "available",
            selectionScope = "retained samples after time/thread/subtree filtering",
            selectedFunction = function,
            scope = "self samples in the selected physical containing function; inline contexts annotate each sample once",
            processSamples, retainedSamples = all.Samples, selectedSelfSamples = scope, resolvedLineSamples = resolved, unresolvedLineSamples = scope - resolved,
            missingStackSamplesInProcess = processMissingStacks, unresolvedLeafSamplesInProcess = processUnresolvedLeafSamples,
            missingStackSamplesInRetained = samples.Where(s => s.Key.Failure == "missing_stack").Sum(s => s.Value),
            unresolvedLeafSamplesInRetained = samples.Where(s => !all.Frames[s.Key.FunctionId].Resolved).Sum(s => s.Value),
            unselectedSamples = all.Samples - scope,
            inlineSamplesReported = rows.Values.Where(r => r.Mapping.InlineStatus == "available").Sum(r => r.Count),
            sourceTextVerified = false,
            semantics = "rows are disjoint self attribution to the deepest known inline location; contexts are outer-to-inner (parent location represents call site), never summed as extra samples; no inclusive line attribution or source text fetched",
            rows = sorted.Take(top).Select(r => new {
                status = r.Value.Mapping.Status, inlineStatus = r.Value.Mapping.InlineStatus,
                location = r.Value.Mapping.Location, contexts = r.Value.Mapping.Contexts,
                selfSamples = r.Value.Count, percentOfFunctionSelf = scope == 0 ? 0 : Math.Round(100.0 * r.Value.Count / scope, 4),
                percentOfProcessSamples = Math.Round(100.0 * r.Value.Count / processSamples, 4),
                percentOfRetainedSamples = all.Samples == 0 ? 0 : Math.Round(100.0 * r.Value.Count / all.Samples, 4),
                addresses = r.Value.Addresses.Order().Take(4).Select(a => "0x" + a.ToString("x")).ToArray(),
                addressesOmitted = Math.Max(0, r.Value.Addresses.Count - 4)
            }).ToArray(),
            rowsOmitted = Math.Max(0, sorted.Length - top), samplesInOmittedRows = sorted.Skip(top).Sum(r => r.Value.Count),
            inlineUncertainty = "none_reported does not prove absence of inlining; optimized/missing PDB records can limit attribution"
        };
    }
    internal static void SelfTest()
    {
        void Check(bool ok) { if (!ok) throw new Exception("source mapping self-test failed"); }
        NativeFrame Frame(uint id, uint parent, uint line) => new() { Id = id, Parent = parent, Name = "f" + id, File = "fixture.cpp", Line = line };
        var result = new NativeResult { Count = 3, Frames = [Frame(1, 0, 10), Frame(3, 2, 30), Frame(2, 1, 20)] };
        var mapping = Normalize(result);
        Check(mapping.Status == "resolved" && mapping.Location?.Line == 30 && mapping.Contexts.Select(s => s.SymbolId).SequenceEqual(new uint[] { 1, 2, 3 }));
        result.Frames[1].Parent = 1;
        Check(Normalize(result).Status == "inline_chain_ambiguous");
        result.Frames[1] = result.Frames[2];
        Check(Normalize(result).Status == "inline_chain_ambiguous");
        result.Count = 1; result.Frames[0].Line = 0;
        Check(Normalize(result).Status == "source_unavailable");
        result.Flags = 2;
        Check(Normalize(result).InlineStatus == "truncated" && Normalize(result).Location == null);
        result.Count = 18;
        Check(Normalize(result).Status == "source_unavailable");
    }
    public void Dispose() { foreach (var session in sessions.Values) if (session.Handle != IntPtr.Zero) bh_dia_close(session.Handle); }
}
