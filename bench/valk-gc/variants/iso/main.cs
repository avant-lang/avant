// modified from upstream: main takes scenario numbers and runs those, in the
// order given (no argument: the seven scenarios in upstream's order,
// printing what upstream prints; `3 4` isolates scenario 4; `0` runs
// nothing); scenario 8, "reverse live links", is ADDED BY AVANT and is not
// part of Valk's suite (scenario 5 with the rotation, 2 stores a round,
// replaced by an in-place reversal, 200,000 stores a round). Nothing else is
// changed. The headline rows are measured with the unmodified file, never
// with this one.
// Unmodified original: ../../upstream/main.cs, from
// Valk's examples/bench/gc/main.cs (https://github.com/valk-lang/valk, tag
// 0.8.5, commit 33224300; MIT, (c) 2024 Lorenz Vandevelde; license in
// ../../upstream/LICENSE).
//
using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

sealed class Node
{
    public Node Next;
}

sealed class Tree
{
    public Tree Left;
    public Tree Right;

    public static Tree Build(int depth)
    {
        if (depth <= 0)
            return new Tree();
        return new Tree {
            Left = Build(depth - 1),
            Right = Build(depth - 1),
        };
    }

    public long Check()
    {
        long result = 1;
        if (Left != null)
            result += Left.Check();
        if (Right != null)
            result += Right.Check();
        return result;
    }
}

static class Program
{
    static Node sink;
    static Node keep;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static Node HeapNode()
    {
        var node = new Node();
        sink = node;
        return node;
    }

    static Node Chain(int count)
    {
        var first = new Node();
        var previous = first;
        for (int i = 1; i < count; i++) {
            var node = new Node();
            previous.Next = node;
            previous = node;
        }
        return first;
    }

    static int ChainLength(Node node)
    {
        int count = 0;
        while (node != null) {
            count++;
            node = node.Next;
        }
        return count;
    }

    static long Microseconds(Stopwatch watch)
    {
        return watch.ElapsedTicks * 1_000_000 / Stopwatch.Frequency;
    }

    static void PrintTime(long microseconds)
    {
        Console.WriteLine($"time_us:  {microseconds}");
        Console.WriteLine($"time_ms:  {microseconds / 1000}");
    }

    static long MemoryKB()
    {
        return GC.GetTotalMemory(false) / 1024;
    }

    static void Header(string name)
    {
        Console.WriteLine();
        Console.WriteLine($"=== {name} ===");
    }

    static void FullGC()
    {
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, false);
        GC.WaitForPendingFinalizers();
    }

    static void CollectN(int count)
    {
        for (int i = 0; i < count; i++)
            FullGC();
    }

    static void BenchShortLived()
    {
        Header("short-lived alloc");
        const int amount = 10_000_000;
        var root = new Node();

        var watch = Stopwatch.StartNew();
        for (int i = amount; i > 0; i--) {
            var node = HeapNode();
            if (i == 1)
                root.Next = node;
        }
        watch.Stop();
        long us = Microseconds(watch);
        CollectN(2);

        Console.WriteLine($"objects:  {amount}");
        PrintTime(us);
        Console.WriteLine($"rate:     {(long)amount * 1000 / (us + 1)} allocs/ms");
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {ChainLength(root)}");
        GC.KeepAlive(root);
    }

    static void BenchStableCollect()
    {
        Header("stable-heap forced collects");
        const int live = 500_000;
        const int collects = 500;
        var list = Chain(live);
        FullGC();

        var watch = Stopwatch.StartNew();
        for (int i = 0; i < collects; i++) {
            HeapNode();
            FullGC();
        }
        watch.Stop();
        long us = Microseconds(watch);

        Console.WriteLine($"live:     {live}");
        Console.WriteLine($"collects: {collects}");
        PrintTime(us);
        Console.WriteLine($"rate:     {(long)collects * 1_000_000 / (us + 1)} collects/s");
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {(ChainLength(list) == live ? "OK" : "FAIL")}");
        GC.KeepAlive(list);
    }

    static void BenchBuildChain()
    {
        Header("build long-lived chain");
        const int amount = 5_000_000;

        var watch = Stopwatch.StartNew();
        var list = Chain(amount);
        watch.Stop();
        long us = Microseconds(watch);
        keep = list;

        Console.WriteLine($"objects:  {amount}");
        PrintTime(us);
        Console.WriteLine($"rate:     {(long)amount * 1000 / (us + 1)} allocs/ms");
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {(ChainLength(list) == amount ? "OK" : "FAIL")}");
    }

    static void BenchFreeChain()
    {
        Header("free long-lived chain");
        var list = keep;
        if (list == null) {
            Console.WriteLine("skipped: no chain from build step");
            return;
        }
        int amount = ChainLength(list);
        int check = amount;

        var watch = Stopwatch.StartNew();
        keep = null;
        list = null;
        CollectN(4);
        watch.Stop();
        long us = Microseconds(watch);

        Console.WriteLine($"objects:  {amount}");
        PrintTime(us);
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {check}");
    }

    static void BenchMutateLinks()
    {
        Header("mutate live links");
        const int live = 200_000;
        const int rounds = 50;
        var list = Chain(live);
        FullGC();

        var watch = Stopwatch.StartNew();
        for (int round = 0; round < rounds; round++) {
            var head = list;
            var second = head.Next;
            if (second == null)
                break;
            head.Next = null;
            var tail = second;
            while (tail.Next != null)
                tail = tail.Next;
            tail.Next = head;
            list = second;
        }
        watch.Stop();
        long us = Microseconds(watch);
        FullGC();

        Console.WriteLine($"live:     {live}");
        Console.WriteLine($"rounds:   {rounds}");
        PrintTime(us);
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {(ChainLength(list) == live ? "OK" : "FAIL")}");
        GC.KeepAlive(list);
    }

    static void BenchChurnWithLive()
    {
        Header("short-lived churn with large live set");
        const int live = 500_000;
        const int churn = 5_000_000;
        var list = Chain(live);
        FullGC();

        var watch = Stopwatch.StartNew();
        for (int i = churn; i > 0; i--)
            HeapNode();
        FullGC();
        watch.Stop();
        long us = Microseconds(watch);

        Console.WriteLine($"live:     {live}");
        Console.WriteLine($"churn:    {churn}");
        PrintTime(us);
        Console.WriteLine($"rate:     {(long)churn * 1000 / (us + 1)} allocs/ms");
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {(ChainLength(list) == live ? "OK" : "FAIL")}");
        GC.KeepAlive(list);
    }

    static void BenchTreeChurn()
    {
        Header("tree churn");
        const int depth = 14;
        const int trees = 400;
        int nodesPerTree = (1 << (depth + 1)) - 1;

        var watch = Stopwatch.StartNew();
        long check = 0;
        for (int i = 0; i < trees; i++)
            check += Tree.Build(depth).Check();
        FullGC();
        watch.Stop();
        long us = Microseconds(watch);

        Console.WriteLine($"depth:    {depth}");
        Console.WriteLine($"trees:    {trees}");
        Console.WriteLine($"nodes:    ~{trees * nodesPerTree}");
        PrintTime(us);
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {check}");
    }

    // 8) ADDED BY AVANT, not part of Valk's suite: reverse a live list in place.
    static void BenchReverseLinks()
    {
        Header("reverse live links");
        const int live = 200_000;
        const int rounds = 50;
        var list = Chain(live);
        FullGC();

        var watch = Stopwatch.StartNew();
        for (int round = 0; round < rounds; round++) {
            Node previous = null;
            var current = list;
            while (current != null) {
                var next = current.Next;
                current.Next = previous;
                previous = current;
                current = next;
            }
            list = previous;
        }
        watch.Stop();
        long us = Microseconds(watch);
        FullGC();

        Console.WriteLine($"live:     {live}");
        Console.WriteLine($"rounds:   {rounds}");
        Console.WriteLine($"stores:   {live * rounds}");
        PrintTime(us);
        Console.WriteLine($"mem_kb:   {MemoryKB()}");
        Console.WriteLine($"verify:   {(ChainLength(list) == live ? "OK" : "FAIL")}");
        GC.KeepAlive(list);
    }

    static void RunScenario(int which)
    {
        switch (which) {
            case 1: BenchShortLived(); break;
            case 2: BenchStableCollect(); break;
            case 3: BenchBuildChain(); break;
            case 4: BenchFreeChain(); break;
            case 5: BenchMutateLinks(); break;
            case 6: BenchChurnWithLive(); break;
            case 7: BenchTreeChurn(); break;
            case 8: BenchReverseLinks(); break;
        }
    }

    public static void Main(string[] args)
    {
        Console.WriteLine("C# GC bench suite");
        Console.WriteLine($"mem_kb before: {MemoryKB()}");

        if (args.Length < 1) {
            for (int which = 1; which <= 7; which++)
                RunScenario(which);
        }
        foreach (var arg in args)
            RunScenario(int.Parse(arg));

        Console.WriteLine();
        Console.WriteLine("=== done ===");
        Console.WriteLine($"mem_kb after: {MemoryKB()}");
        GC.KeepAlive(sink);
    }
}
