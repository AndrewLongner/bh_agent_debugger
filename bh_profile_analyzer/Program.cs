using System.Globalization;
using System.IO.Compression;
using System.Text.Json;
using Microsoft.Diagnostics.Symbols;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Etlx;
using Microsoft.Diagnostics.Tracing.Parsers.Kernel;

namespace BhProfileAnalyzer;

internal sealed record Frame(string Id, string Module, string Name, bool Resolved);
internal sealed class Counts
{
    public long Self, Inclusive;
}
internal sealed class Node(int id, int? parent, string frame)
{
    public int Id = id;
    public int? Parent = parent;
    public string Frame = frame;
    public long Self, Inclusive;
    public Dictionary<string, Node> Children = new(StringComparer.Ordinal);
}
internal sealed class Aggregation
{
    public readonly Dictionary<string, Frame> Frames = new(StringComparer.Ordinal);
    public readonly Dictionary<string, Counts> Functions = new(StringComparer.Ordinal);
    public readonly List<Node> Nodes = new();
    public readonly Node Root = new(0, null, "[root]");
    public long Samples;
    public Aggregation() { Nodes.Add(Root); }
    public void Add(IReadOnlyList<Frame> leafFirst)
    {
        if (leafFirst.Count == 0) throw new ArgumentException("empty stack");
        if (Nodes.Count + leafFirst.Count > 200000 || Frames.Count + leafFirst.Count > 100000)
            throw new InvalidDataException("analysis graph exceeds safety limit");
        ++Samples;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var f in leafFirst)
        {
            Frames.TryAdd(f.Id, f);
            if (!Functions.TryGetValue(f.Id, out var value)) Functions.Add(f.Id, value = new Counts());
            if (seen.Add(f.Id)) ++value.Inclusive; // recursion counts once per sample in flat view
        }
        ++Functions[leafFirst[0].Id].Self;
        var node = Root;
        ++node.Inclusive;
        for (int i = leafFirst.Count - 1; i >= 0; --i)
        {
            var frame = leafFirst[i];
            if (!node.Children.TryGetValue(frame.Id, out var child))
            {
                child = new Node(Nodes.Count, node.Id, frame.Id);
                Nodes.Add(child);
                node.Children.Add(frame.Id, child);
            }
            node = child;
            ++node.Inclusive;
        }
        ++node.Self;
    }
    // Report sections a caller can select with --sections (default: all).
    public static readonly string[] SectionNames = { "functionsBySelf", "functionsByInclusive", "callTree", "hotPaths" };
    public object Report(int top, double? intervalMs, string? subtreeId = null, IReadOnlySet<string>? sections = null)
    {
        double Percent(long count) => Samples == 0 ? 0 : Math.Round(100.0 * count / Samples, 4);
        double? Time(long count) => intervalMs is double ms ? Math.Round(count * ms, 4) : null;
        bool Want(string name) => sections == null || sections.Contains(name);
        object Function(KeyValuePair<string, Counts> item) => new {
            id = item.Key, frame = Frames[item.Key], selfSamples = item.Value.Self, inclusiveSamples = item.Value.Inclusive,
            selfPercent = Percent(item.Value.Self), inclusivePercent = Percent(item.Value.Inclusive),
            selfCpuMsEstimate = Time(item.Value.Self), inclusiveCpuMsEstimate = Time(item.Value.Inclusive) };
        // Subtree scope: a node is caller context when neither it nor any ancestor is the
        // subtree frame. Nodes are appended after their parents, so one forward pass works.
        var context = new bool[Nodes.Count];
        var scopeRoots = new List<Node>();
        var ancestors = new HashSet<string>(StringComparer.Ordinal);
        if (subtreeId != null)
        {
            var inScope = new HashSet<string>(StringComparer.Ordinal);
            foreach (var node in Nodes)
            {
                if (node.Parent is not int p) { context[node.Id] = true; continue; }
                context[node.Id] = context[p] && node.Frame != subtreeId;
                if (context[p] && !context[node.Id]) scopeRoots.Add(node);
                if (!context[node.Id]) inScope.Add(node.Frame);
            }
            // Frames above the root-most subtree frame in EVERY retained sample (and never
            // inside it) are 100%-inclusive caller context; they carry no ranking information.
            foreach (var item in Functions)
                if (Samples > 0 && item.Value.Inclusive == Samples && !inScope.Contains(item.Key)) ancestors.Add(item.Key);
        }
        var ranked = Functions.Where(x => !ancestors.Contains(x.Key)).ToArray();
        // Bounded tree selection: include ancestors before descendants, hottest children first.
        var tree = new List<Node>();
        void Visit(Node node) {
            if (tree.Count >= top) return;
            tree.Add(node);
            foreach (var child in node.Children.Values.OrderByDescending(n => n.Inclusive).ThenBy(n => n.Frame, StringComparer.Ordinal)) Visit(child);
        }
        var starts = subtreeId == null ? new List<Node> { Root }
            : scopeRoots.OrderByDescending(n => n.Inclusive).ThenBy(n => n.Id).ToList();
        foreach (var start in starts) Visit(start);
        var startIds = starts.Select(n => n.Id).ToHashSet();
        var paths = Nodes.Where(n => n.Self > 0).OrderByDescending(n => n.Self).ThenBy(n => n.Id).Take(top).Select(n => {
            var frames = new List<Frame>();
            var current = n;
            bool trim = subtreeId != null && !context[n.Id];
            while (current.Parent is int parent && !(trim && context[current.Id])) { frames.Add(Frames[current.Frame]); current = Nodes[parent]; }
            int above = 0;
            while (current.Parent is int up) { ++above; current = Nodes[up]; }
            frames.Reverse();
            return new { frames, framesAboveOmitted = above, samples = n.Self, percent = Percent(n.Self), cpuMsEstimate = Time(n.Self) };
        }).ToArray();
        return new {
            totalFunctions = Functions.Count,
            functionsBySelf = Want("functionsBySelf") ? ranked.OrderByDescending(x => x.Value.Self).ThenBy(x => x.Key, StringComparer.Ordinal).Take(top).Select(Function).ToArray() : null,
            functionsByInclusive = Want("functionsByInclusive") ? ranked.OrderByDescending(x => x.Value.Inclusive).ThenBy(x => x.Key, StringComparer.Ordinal).Take(top).Select(Function).ToArray() : null,
            functionRowsOmittedPerView = Math.Max(0, ranked.Length - top),
            callTreeScope = subtreeId == null ? "process" : "subtree",
            callTree = !Want("callTree") ? null : tree.Select(n => new { id = n.Id, parentId = subtreeId != null && startIds.Contains(n.Id) ? null : n.Parent,
                frame = n.Id == 0 ? new Frame("[root]", "", "[root]", false) : Frames[n.Frame],
                selfSamples = n.Self, inclusiveSamples = n.Inclusive, inclusivePercent = Percent(n.Inclusive),
                selfCpuMsEstimate = Time(n.Self), inclusiveCpuMsEstimate = Time(n.Inclusive) }).ToArray(),
            callTreeNodesOmitted = Nodes.Count - tree.Count,
            hotPaths = Want("hotPaths") ? paths : null, hotPathsOmitted = Math.Max(0, Nodes.Count(n => n.Self > 0) - top),
            subtreeScope = subtreeId == null ? null : new {
                frameId = subtreeId, callerContexts = scopeRoots.Count, samplesUnderCallerContexts = scopeRoots.Sum(n => n.Inclusive),
                callerContextsShownInCallTree = scopeRoots.Count(n => tree.Contains(n)),
                alwaysAboveSubtreeFramesOmittedFromRankings = ancestors.Count,
                semantics = "hot paths start at the root-most subtree frame (framesAboveOmitted counts trimmed callers); call tree starts at each root-most subtree caller context (parentId null); frames above the subtree in every retained sample and never inside it are omitted from rankings, while totalFunctions still counts them" },
            sectionsOmitted = sections == null ? Array.Empty<string>() : SectionNames.Where(s => !sections.Contains(s)).ToArray()
        };
    }
}
// Retain a bounded diagnostic summary in JSON instead of mixing console logs
// into the machine-readable stdout protocol. Missing PDBs are also reflected in
// unresolved sample counts; they are not reported as successfully resolved.
internal sealed class Diagnostics : TextWriter
{
    public override System.Text.Encoding Encoding => System.Text.Encoding.UTF8;
    readonly System.Text.StringBuilder line = new();
    public long WarningCount;
    public List<string> Warnings = new();
    public override void Write(char value)
    {
        if (value == '\n')
        {
            var text = line.ToString(); line.Clear();
            if (text.Contains("WARNING", StringComparison.OrdinalIgnoreCase) || text.Contains("failed", StringComparison.OrdinalIgnoreCase))
            {
                ++WarningCount;
                if (Warnings.Count < 8) Warnings.Add(text);
            }
        }
        else if (value != '\r' && line.Length < 256) line.Append(value);
    }
}
internal sealed record Options(string Input, int Pid, int Top, string? Symbols, string? Function, FilterOptions Filters, IReadOnlySet<string>? Sections);
internal static class Program
{
    const long MaxInput = 512L * 1024 * 1024;
    static readonly JsonSerializerOptions JsonOptions = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    static Options Parse(string[] args)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (int i = 0; i < args.Length; i += 2)
        {
            if (i + 1 == args.Length || !new[] { "--input", "--pid", "--top", "--symbols", "--function", "--start-ms", "--end-ms", "--thread", "--subtree", "--sections" }.Contains(args[i]) || !values.TryAdd(args[i], args[i + 1]))
                throw new ArgumentException("usage: --input <etl|diagsession> --pid <pid> [--top <1..50>] [--symbols <local-directory>] [--function <exact-name-or-id>] [--start-ms <ms>] [--end-ms <ms>] [--thread <tid>] [--subtree <exact-name-or-id>] [--sections <comma-list>]");
        }
        if (!values.TryGetValue("--input", out var input) || !values.TryGetValue("--pid", out var pidText) ||
            !int.TryParse(pidText, NumberStyles.None, CultureInfo.InvariantCulture, out var pid) || pid <= 0)
            throw new ArgumentException("explicit positive PID and input required");
        int top = 20;
        if (values.TryGetValue("--top", out var topText) && (!int.TryParse(topText, NumberStyles.None, CultureInfo.InvariantCulture, out top) || top < 1 || top > 50))
            throw new ArgumentException("top must be 1..50");
        var symbols = values.GetValueOrDefault("--symbols");
        if (symbols != null) {
            symbols = Path.GetFullPath(symbols);
            if (symbols.StartsWith(@"\\", StringComparison.Ordinal) || symbols.Contains(';') || symbols.Contains('*') || !Directory.Exists(symbols))
                throw new ArgumentException("symbols must be one existing local directory, not a symbol server");
        }
        input = Path.GetFullPath(input);
        if (input.StartsWith(@"\\", StringComparison.Ordinal)) throw new ArgumentException("input must be local");
        var function = values.GetValueOrDefault("--function");
        if (function != null && (string.IsNullOrWhiteSpace(function) || function.Length > 1024 || function.Any(char.IsControl)))
            throw new ArgumentException("function must be an exact nonempty name or trace-local id, at most 1024 characters");
        HashSet<string>? sections = null;
        if (values.TryGetValue("--sections", out var sectionText))
        {
            sections = new HashSet<string>(StringComparer.Ordinal);
            foreach (var part in sectionText.Split(','))
                if (!Aggregation.SectionNames.Contains(part) || !sections.Add(part))
                    throw new ArgumentException("sections must be a nonempty comma-separated subset of " + string.Join(",", Aggregation.SectionNames) + " without duplicates");
        }
        return new Options(input, pid, top, symbols, function, FilterOptions.Parse(values), sections);
    }
    static string PrepareInput(string input, string scratch)
    {
        var file = new FileInfo(input);
        if (!file.Exists || file.Length == 0 || file.Length > MaxInput) throw new InvalidDataException("input missing, empty, or over 512 MiB");
        var target = Path.Combine(scratch, "input.etl");
        if (file.Extension.Equals(".etl", StringComparison.OrdinalIgnoreCase)) File.Copy(input, target, false);
        else if (file.Extension.Equals(".diagsession", StringComparison.OrdinalIgnoreCase))
        {
            using var archive = ZipFile.OpenRead(input);
            if (archive.Entries.Count > 4096) throw new InvalidDataException("too many archive entries");
            var entries = archive.Entries.Where(e => e.FullName.EndsWith(".etl", StringComparison.OrdinalIgnoreCase)).ToArray();
            if (entries.Length != 1) throw new InvalidDataException("expected exactly one ETL; explicitly extract/select the intended trace for multi-ETL sessions");
            var entry = entries[0];
            if (entry.Length <= 0 || entry.Length > MaxInput) throw new InvalidDataException("ETL archive entry outside size limit");
            // Never use archive entry names as destination paths (ZIP traversal safe).
            using var source = entry.Open();
            using var dest = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            byte[] buffer = new byte[65536];
            long copied = 0;
            int read;
            while ((read = source.Read(buffer)) != 0) {
                copied += read;
                if (copied > MaxInput || copied > entry.Length) throw new InvalidDataException("inflated ETL exceeds declared limit");
                dest.Write(buffer, 0, read);
            }
            if (copied != entry.Length) throw new InvalidDataException("incomplete ETL entry");
        }
        else throw new ArgumentException("only .etl and .diagsession are supported");
        return target;
    }
    static object Analyze(Options options, string scratch)
    {
        string etl = PrepareInput(options.Input, scratch);
        string etlx = Path.Combine(scratch, "input.etlx");
        // Never reuse stale ETLX or write next to the source report.
        var diagnostics = new Diagnostics();
        TraceLog.CreateFromEventTraceLogFile(etl, etlx, new TraceLogOptions { ConversionLog = diagnostics });
        using var log = new TraceLog(etlx);
        var candidates = log.Processes.Where(p => p.ProcessID == options.Pid).ToArray();
        if (candidates.Length != 1) throw new InvalidDataException(candidates.Length == 0 ? "PID not present in trace" : "PID reused in trace; ambiguous process lifetime, refusing to merge");
        var process = candidates[0];
        int symbolFailures = 0;
        if (options.Symbols != null)
        {
            using var reader = new SymbolReader(diagnostics, options.Symbols);
            reader.Options = SymbolReaderOptions.CacheOnly | SymbolReaderOptions.NoNGenSymbolCreation;
            reader.SecurityCheck = path => Path.GetFullPath(path).StartsWith(options.Symbols.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase);
            int modules = 0;
            foreach (var module in process.LoadedModules.Select(m => m.ModuleFile).DistinctBy(m => m.ModuleFileIndex))
            {
                if (++modules > 1024) throw new InvalidDataException("module limit exceeded");
                try { log.CodeAddresses.LookupSymbolsForModule(reader, module); }
                catch (Exception ex) { ++symbolFailures; diagnostics.WriteLine("symbol lookup failed: " + ex.Message); }
            }
        }
        using var sources = options.Function == null ? null : new SourceAnalysis(process, options.Symbols);
        var aggregate = new Aggregation();
        var filters = new FilterState(options.Filters);
        var frameCache = new Dictionary<int, Frame>();
        long missingStacks = 0, truncatedStacks = 0, unresolvedLeafSamples = 0, samplesWithUnresolvedFrames = 0;
        double first = double.PositiveInfinity, last = 0;
        foreach (var item in log.Events)
        {
            if (item is not SampledProfileTraceData sample || sample.Process()?.ProcessIndex != process.ProcessIndex) continue;
            var frames = new List<Frame>();
            var stack = sample.CallStack();
            var leafAddress = stack?.CodeAddress;
            if (stack == null) frames.Add(new Frame("[missing-stack]", "", "[missing-stack]", false));
            int depth = 0;
            while (stack != null && depth++ < 128)
            {
                var address = stack.CodeAddress;
                int index = (int)address.CodeAddressIndex;
                if (!frameCache.TryGetValue(index, out var frame))
                {
                    if (frameCache.Count >= 200000) throw new InvalidDataException("code-address limit exceeded");
                    var module = address.ModuleFile;
                    var name = address.FullMethodName;
                    var method = address.Method;
                    bool resolved = method != null && !string.IsNullOrEmpty(name);
                    var id = method != null && resolved ? "m" + (int)(module?.ModuleFileIndex ?? ModuleFileIndex.Invalid) + ":f" + (int)method.MethodIndex
                        : "m" + (int)(module?.ModuleFileIndex ?? ModuleFileIndex.Invalid) + ":0x" + address.Address.ToString("x");
                    frame = new Frame(id, Path.GetFileName(module?.FilePath ?? ""), resolved ? name : "0x" + address.Address.ToString("x"), resolved);
                    frameCache.Add(index, frame);
                }
                frames.Add(frame);
                stack = stack.Caller;
            }
            if (stack != null) frames.Add(new Frame("[truncated-root]", "", "[truncated-root]", false));
            if (filters.Classify(sample.TimeStampRelativeMSec, sample.ThreadID, (int)(sample.Thread()?.ThreadIndex ?? ThreadIndex.Invalid), frames) != FilterDecision.Retained) continue;
            first = Math.Min(first, sample.TimeStampRelativeMSec); last = Math.Max(last, sample.TimeStampRelativeMSec);
            if (leafAddress == null) ++missingStacks;
            if (stack != null) ++truncatedStacks;
            if (!frames[0].Resolved) ++unresolvedLeafSamples;
            if (frames.Any(f => !f.Resolved)) ++samplesWithUnresolvedFrames;
            aggregate.Add(frames);
            sources?.Add(leafAddress, frames[0], sample.TimeStampRelativeMSec);
        }
        filters.Validate();
        double ms = log.SampleProfileInterval.TotalMilliseconds;
        double? interval = double.IsFinite(ms) && ms > 0 ? ms : null;
        return new {
            schemaVersion = 1, ok = true, backend = "TraceEvent-3.1.28", input = options.Input,
            process = new { pid = options.Pid, processIndex = (int)process.ProcessIndex, name = process.Name,
                startMs = process.StartTimeRelativeMsec, endMs = process.EndTimeRelativeMsec },
            metrics = new { samples = aggregate.Samples, sampleIntervalMs = interval,
                cpuMsEstimate = interval * aggregate.Samples, firstSampleMs = aggregate.Samples == 0 ? (double?)null : first, lastSampleMs = aggregate.Samples == 0 ? (double?)null : last,
                timingBasis = "TraceLog.SampleProfileInterval; library may use default; constant interval assumed; CPU estimate, not elapsed time or call duration" },
            filters = filters.Report(options.Top),
            quality = new { eventsLost = log.EventsLost, eventsLostScope = "entire_trace", missingStacks, truncatedStacks, unresolvedLeafSamples, samplesWithUnresolvedFrames,
                symbolLookupExceptions = symbolFailures, symbols = options.Symbols == null ? "not_requested" : "local_only",
                diagnosticWarningCount = diagnostics.WarningCount, diagnosticWarnings = diagnostics.Warnings,
                diagnosticWarningsOmitted = diagnostics.WarningCount - diagnostics.Warnings.Count,
                percentagesDenominator = "retainedSamples (after filters); includes retained missing stacks; see filters for excluded and indeterminate samples" },
            analysis = aggregate.Report(options.Top, interval, filters.SubtreeFrameId, options.Sections),
            sourceDrilldown = sources?.Report(options.Function!, aggregate, options.Top, filters.ProcessSamples, filters.ProcessQuality.MissingStacks, filters.ProcessQuality.UnresolvedLeafSamples)
        };
    }
    static int SelfTest()
    {
        void Check(bool condition) { if (!condition) throw new Exception("analyzer self-test failed"); }
        SourceAnalysis.SelfTest();
        FilterState.SelfTest();
        var data = new Aggregation();
        var a = new Frame("a", "fixture", "a", true); var b = new Frame("b", "fixture", "b", true);
        data.Add(new[] { a, a, b }); data.Add(new[] { b });
        Check(data.Samples == 2 && data.Functions["a"].Self == 1 && data.Functions["a"].Inclusive == 1 && data.Functions["b"].Inclusive == 2);
        Check(data.Root.Inclusive == 2 && data.Nodes.Sum(n => n.Self) == 2);
        using var report = JsonDocument.Parse(JsonSerializer.Serialize(data.Report(2, 1.0), JsonOptions));
        Check(report.RootElement.GetProperty("callTree").GetArrayLength() == 2);
        Check(report.RootElement.GetProperty("callTreeNodesOmitted").GetInt32() == data.Nodes.Count - 2);
        Check(report.RootElement.GetProperty("hotPaths")[0].GetProperty("frames")[0].GetProperty("id").GetString() == "b");
        // Subtree scope: main is above s in every sample; x is a partial caller context.
        var s = new Frame("s", "fixture", "s", true); var c = new Frame("c", "fixture", "c", true); var d = new Frame("d", "fixture", "d", true);
        var x = new Frame("x", "fixture", "x", true); var main = new Frame("main", "fixture", "main", true);
        var scoped = new Aggregation();
        scoped.Add(new[] { c, s, main }); scoped.Add(new[] { d, s, s, main }); scoped.Add(new[] { s, x, main });
        using var sub = JsonDocument.Parse(JsonSerializer.Serialize(scoped.Report(10, 1.0, "s"), JsonOptions));
        var subRoot = sub.RootElement;
        Check(!subRoot.GetProperty("functionsByInclusive").EnumerateArray().Any(e => e.GetProperty("id").GetString() == "main"));
        Check(subRoot.GetProperty("functionsByInclusive").EnumerateArray().Any(e => e.GetProperty("id").GetString() == "x"));
        Check(subRoot.GetProperty("hotPaths").GetArrayLength() == 3 && subRoot.GetProperty("hotPaths").EnumerateArray().All(p => p.GetProperty("frames")[0].GetProperty("id").GetString() == "s"));
        Check(subRoot.GetProperty("hotPaths").EnumerateArray().Sum(p => p.GetProperty("framesAboveOmitted").GetInt32()) == 4);
        var scope = subRoot.GetProperty("subtreeScope");
        Check(scope.GetProperty("callerContexts").GetInt32() == 2 && scope.GetProperty("samplesUnderCallerContexts").GetInt64() == 3 && scope.GetProperty("alwaysAboveSubtreeFramesOmittedFromRankings").GetInt32() == 1);
        Check(subRoot.GetProperty("callTree")[0].GetProperty("parentId").ValueKind == JsonValueKind.Null && subRoot.GetProperty("callTree")[0].GetProperty("frame").GetProperty("id").GetString() == "s");
        using var only = JsonDocument.Parse(JsonSerializer.Serialize(scoped.Report(10, 1.0, "s", new HashSet<string> { "hotPaths" }), JsonOptions));
        Check(only.RootElement.GetProperty("callTree").ValueKind == JsonValueKind.Null && only.RootElement.GetProperty("functionsBySelf").ValueKind == JsonValueKind.Null);
        Check(only.RootElement.GetProperty("hotPaths").GetArrayLength() == 3 && only.RootElement.GetProperty("sectionsOmitted").GetArrayLength() == 3);
        Check(report.RootElement.GetProperty("callTreeScope").GetString() == "process" && report.RootElement.GetProperty("hotPaths")[0].GetProperty("framesAboveOmitted").GetInt32() == 0);
        Check(Parse(new[] { "--input", "a.etl", "--pid", "1", "--sections", "hotPaths,callTree" }).Sections!.Count == 2);
        Check(Parse(new[] { "--input", "a.etl", "--pid", "123" }).Top == 20);
        foreach (var args in new[] { new[] { "--pid", "0" }, new[] { "--input", "a.etl", "--pid", "1", "--top", "0" },
            new[] { "--input", "a.etl", "--pid", "1", "--pid", "2" }, new[] { "--input", "a.etl", "--pid", "1", "--sections", "" },
            new[] { "--input", "a.etl", "--pid", "1", "--sections", "hotPaths,hotPaths" }, new[] { "--input", "a.etl", "--pid", "1", "--sections", "bogus" } })
        { bool rejected = false; try { Parse(args); } catch (ArgumentException) { rejected = true; } Check(rejected); }
        string testDirectory = Path.Combine(Path.GetTempPath(), "bh-profile-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(testDirectory);
        try
        {
            string scratch = Path.Combine(testDirectory, "scratch"); Directory.CreateDirectory(scratch);
            string zip = Path.Combine(testDirectory, "traversal.diagsession");
            using (var archive = ZipFile.Open(zip, ZipArchiveMode.Create))
            using (var stream = archive.CreateEntry("../escape.etl").Open()) stream.Write(new byte[] { 1, 2, 3 });
            string extracted = PrepareInput(zip, scratch);
            Check(extracted == Path.Combine(scratch, "input.etl") && new FileInfo(extracted).Length == 3);
            Check(!File.Exists(Path.Combine(testDirectory, "escape.etl")));
            string multi = Path.Combine(testDirectory, "multiple.diagsession");
            using (var archive = ZipFile.Open(multi, ZipArchiveMode.Create))
            {
                archive.CreateEntry("a.etl"); archive.CreateEntry("b.etl");
            }
            bool rejected = false;
            try { PrepareInput(multi, scratch); } catch (InvalidDataException) { rejected = true; }
            Check(rejected);
            File.Delete(extracted);
            string empty = Path.Combine(testDirectory, "empty.diagsession");
            using (var archive = ZipFile.Open(empty, ZipArchiveMode.Create)) archive.CreateEntry("metadata.xml");
            rejected = false;
            try { PrepareInput(empty, scratch); } catch (InvalidDataException) { rejected = true; }
            Check(rejected);
        }
        finally { Directory.Delete(testDirectory, true); }
        using var unknownTime = JsonDocument.Parse(JsonSerializer.Serialize(data.Report(1, null), JsonOptions));
        Check(unknownTime.RootElement.GetProperty("functionsBySelf")[0].GetProperty("selfCpuMsEstimate").ValueKind == JsonValueKind.Null);
        Console.WriteLine("{\"ok\":true,\"tests\":\"aggregation,recursion,tree,bounds,arguments,zip-safety,ambiguous-archives,unknown-time,inline-chain-normalization,filter-accounting,subtree-scope,sections\"}");
        return 0;
    }
    static int Main(string[] args)
    {
        string? scratch = null;
        try
        {
            if (args.SequenceEqual(new[] { "--self-test" })) return SelfTest();
            var options = Parse(args);
            // Do not inherit symbol servers or download PDBs implicitly.
            Environment.SetEnvironmentVariable("_NT_SYMBOL_PATH", options.Symbols ?? "");
            Environment.SetEnvironmentVariable("_NT_ALT_SYMBOL_PATH", "");
            scratch = Path.Combine(Path.GetTempPath(), "bh-profile-analysis-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(scratch);
            var result = Analyze(options, scratch);
            string json = JsonSerializer.Serialize(result, JsonOptions);
            if (System.Text.Encoding.UTF8.GetByteCount(json) > 1024 * 1024) throw new InvalidDataException("JSON output exceeds 1 MiB bound; reduce --top");
            Console.WriteLine(json);
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { schemaVersion = 1, ok = false, error = ex.Message }, JsonOptions));
            return 1;
        }
        finally
        {
            if (scratch != null) try { Directory.Delete(scratch, true); } catch (Exception ex) { Console.Error.WriteLine("scratch cleanup failed: " + ex.Message); }
        }
    }
}
