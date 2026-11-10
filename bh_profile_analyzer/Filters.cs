using System.Globalization;

namespace BhProfileAnalyzer;

internal sealed record FilterOptions(double? StartMs, double? EndMs, int? ThreadId, string? Subtree)
{
    public static FilterOptions Parse(IReadOnlyDictionary<string, string> values)
    {
        double? Time(string key)
        {
            if (!values.TryGetValue(key, out var text)) return null;
            if (!double.TryParse(text, NumberStyles.AllowDecimalPoint | NumberStyles.AllowExponent, CultureInfo.InvariantCulture, out var value) ||
                !double.IsFinite(value) || value < 0 || value > 1e12)
                throw new ArgumentException(key + " must be a finite nonnegative decimal in trace-relative milliseconds (max 1e12)");
            return value;
        }
        double? start = Time("--start-ms"), end = Time("--end-ms");
        if (end.HasValue && end.Value <= (start ?? 0)) throw new ArgumentException("end-ms must be greater than start-ms (default 0)");
        int? tid = null;
        if (values.TryGetValue("--thread", out var thread))
        {
            if (!int.TryParse(thread, NumberStyles.None, CultureInfo.InvariantCulture, out var number) || number <= 0)
                throw new ArgumentException("thread must be a positive native TID, not a thread-list index");
            tid = number;
        }
        string? subtree = values.GetValueOrDefault("--subtree");
        if (subtree != null && (string.IsNullOrWhiteSpace(subtree) || subtree.Length > 1024 || subtree.Any(char.IsControl)))
            throw new ArgumentException("subtree must be an exact physical function name or trace-local frame id (1..1024 characters)");
        return new(start, end, tid, subtree);
    }
}
internal enum FilterDecision { Retained, Time, Thread, Subtree, Indeterminate }
internal sealed class SampleQuality
{
    public long MissingStacks, TruncatedStacks, UnresolvedLeafSamples, SamplesWithUnresolvedFrames;
    public void Add(IReadOnlyList<Frame> frames)
    {
        if (frames[0].Id == "[missing-stack]") ++MissingStacks;
        if (frames.Any(f => f.Id == "[truncated-root]")) ++TruncatedStacks;
        if (!frames[0].Resolved) ++UnresolvedLeafSamples;
        if (frames.Any(f => !f.Resolved)) ++SamplesWithUnresolvedFrames;
    }
    public object Report() => new { missingStacks = MissingStacks, truncatedStacks = TruncatedStacks,
        unresolvedLeafSamples = UnresolvedLeafSamples, samplesWithUnresolvedFrames = SamplesWithUnresolvedFrames };
}
internal sealed class FilterState(FilterOptions options)
{
    public long ProcessSamples, RetainedSamples, ExcludedTime, ExcludedThread, ExcludedSubtree, IndeterminateSubtree;
    public readonly SampleQuality ProcessQuality = new(), IndeterminateQuality = new();
    readonly HashSet<string> roots = new(StringComparer.Ordinal);
    public string? SubtreeFrameId => roots.Count == 1 ? roots.First() : null;
    readonly HashSet<int> selectedThreadLifetimes = new();
    readonly Dictionary<int, (long Total, long Retained)> threads = new();
    static bool Matches(Frame frame, string selector) => !frame.Id.StartsWith('[') &&
        (frame.Id == selector || (frame.Resolved && frame.Name == selector));
    public FilterDecision Classify(double time, int threadId, int threadIndex, IReadOnlyList<Frame> frames)
    {
        if (++ProcessSamples > 5000000) throw new InvalidDataException("process sample limit exceeded");
        ProcessQuality.Add(frames);
        if (threads.Count >= 65536 && !threads.ContainsKey(threadId)) throw new InvalidDataException("thread bound exceeded");
        threads.TryGetValue(threadId, out var counts); counts.Total++;
        threads[threadId] = counts;
        if (options.ThreadId == threadId)
        {
            if (threadIndex < 0) throw new InvalidDataException("selected thread lifetime unavailable");
            selectedThreadLifetimes.Add(threadIndex);
            if (selectedThreadLifetimes.Count > 1) throw new InvalidDataException("selected native TID reused in trace; refusing to merge lifetimes");
        }
        // Resolve selector identity across the whole process, not just the time
        // window. Narrowing a filter must never turn an ambiguous name into a guess.
        bool found = false;
        if (options.Subtree != null)
        {
            foreach (var frame in frames.Where(f => Matches(f, options.Subtree)))
            {
                found = true; roots.Add(frame.Id);
                if (roots.Count > 1) throw new InvalidDataException("ambiguous subtree name; use exact trace-local frame id");
            }
        }
        if (time < (options.StartMs ?? 0) || (options.EndMs.HasValue && time >= options.EndMs.Value)) { ++ExcludedTime; return FilterDecision.Time; }
        if (options.ThreadId.HasValue && threadId != options.ThreadId.Value) { ++ExcludedThread; return FilterDecision.Thread; }
        if (options.Subtree != null && !found)
        {
            if (frames.Any(f => !f.Resolved))
            {
                ++IndeterminateSubtree; IndeterminateQuality.Add(frames);
                return FilterDecision.Indeterminate;
            }
            ++ExcludedSubtree; return FilterDecision.Subtree;
        }
        ++RetainedSamples; counts.Retained++; threads[threadId] = counts;
        return FilterDecision.Retained;
    }
    public void Validate()
    {
        if (ProcessSamples == 0) throw new InvalidDataException("no sampled CPU events for selected process");
        if (options.ThreadId.HasValue && selectedThreadLifetimes.Count == 0) throw new InvalidDataException("selected thread has no sampled CPU events in process");
        if (options.Subtree != null && roots.Count == 0) throw new InvalidDataException("subtree selector unresolved or absent; matching symbols or an exact frame id are required");
        if (ProcessSamples != RetainedSamples + ExcludedTime + ExcludedThread + ExcludedSubtree + IndeterminateSubtree)
            throw new InvalidDataException("filter accounting mismatch");
    }
    public object Report(int top) => new {
        status = RetainedSamples > 0 ? "retained_samples" : IndeterminateSubtree > 0 ? "no_confirmed_matches_with_indeterminate" : "empty_selection",
        startMs = options.StartMs, endMs = options.EndMs, threadId = options.ThreadId, subtree = options.Subtree,
        subtreeFrameId = roots.SingleOrDefault(),
        timeBasis = "trace-relative milliseconds; half-open [start,end); missing start=0, missing end=unbounded",
        combination = "AND; disjoint exclusion precedence: time, thread, subtree; indeterminate membership reported separately",
        subtreeSemantics = "physical stack contains selected frame, across all caller contexts; full retained stacks preserved; recursive matches count once; not inline-only or a specific call-tree node",
        processSamples = ProcessSamples, retainedSamples = RetainedSamples,
        excludedSamples = ExcludedTime + ExcludedThread + ExcludedSubtree,
        excludedByTime = ExcludedTime, excludedByThread = ExcludedThread, excludedBySubtree = ExcludedSubtree,
        indeterminateSubtreeSamples = IndeterminateSubtree,
        denominator = "retainedSamples for function/tree/path percentages and CPU estimates; indeterminate subtree samples are not confirmed matches and are not in that denominator",
        processQuality = ProcessQuality.Report(), indeterminateSubtreeQuality = IndeterminateQuality.Report(),
        threads = threads.OrderByDescending(t => t.Value.Total).ThenBy(t => t.Key).Take(top)
            .Select(t => new { threadId = t.Key, processSamples = t.Value.Total, retainedSamples = t.Value.Retained }).ToArray(),
        threadRowsOmitted = Math.Max(0, threads.Count - top)
    };
    internal static void SelfTest()
    {
        void Check(bool value) { if (!value) throw new Exception("filter self-test failed"); }
        var a = new Frame("a", "m", "a", true); var b = new Frame("b", "m", "b", true);
        var missing = new Frame("[missing-stack]", "", "[missing-stack]", false);
        var f = new FilterState(new(10, 20, 7, "a"));
        Check(f.Classify(9, 7, 1, [a]) == FilterDecision.Time);
        Check(f.Classify(10, 7, 1, [b, a, a]) == FilterDecision.Retained);
        Check(f.Classify(20, 7, 1, [a]) == FilterDecision.Time);
        Check(f.Classify(11, 8, 2, [a]) == FilterDecision.Thread);
        Check(f.Classify(11, 7, 1, [b]) == FilterDecision.Subtree);
        Check(f.Classify(11, 7, 1, [missing]) == FilterDecision.Indeterminate);
        Check(f.Classify(11, 7, 1, [a, missing]) == FilterDecision.Retained);
        f.Validate();
        Check(f.ProcessSamples == 7 && f.RetainedSamples == 2 && f.IndeterminateSubtree == 1 && f.ProcessQuality.MissingStacks == 1);
        var unfiltered = new FilterState(new(null, null, null, null));
        Check(unfiltered.Classify(0, 7, 1, [missing]) == FilterDecision.Retained); unfiltered.Validate();
        var empty = new FilterState(new(100, 200, null, null)); empty.Classify(1, 7, 1, [a]); empty.Validate(); Check(empty.RetainedSamples == 0);
        bool rejected = false;
        try { f.Classify(12, 7, 9, [a]); } catch (InvalidDataException) { rejected = true; } Check(rejected);
        var ambiguous = new FilterState(new(null, null, null, "a"));
        ambiguous.Classify(1, 1, 1, [a]); rejected = false;
        try { ambiguous.Classify(2, 1, 1, [a with { Id = "other" }]); } catch (InvalidDataException) { rejected = true; } Check(rejected);
        foreach (var values in new[] {
            new Dictionary<string,string> { ["--start-ms"] = "NaN" }, new() { ["--end-ms"] = "Infinity" },
            new() { ["--start-ms"] = "-1" }, new() { ["--start-ms"] = "2", ["--end-ms"] = "2" },
            new() { ["--thread"] = "0" }, new() { ["--subtree"] = " " } })
        { rejected = false; try { FilterOptions.Parse(values); } catch (ArgumentException) { rejected = true; } Check(rejected); }
    }
}
