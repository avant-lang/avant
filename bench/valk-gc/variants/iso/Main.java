// GC microbenchmarks: a Java port of Valk's suite, SCENARIO-ISOLATED, with an
// eighth scenario added by Avant.
//
// modified from ../../Main.java: the program takes scenario numbers and runs
// those, in the order given (no argument: the seven scenarios in upstream's
// order, printing what ../../Main.java prints; `3 4` isolates scenario 4; `0`
// runs nothing); scenario 8, "reverse live links", is ADDED BY AVANT and is
// not part of Valk's suite (scenario 5 with the rotation, 2 stores a round,
// replaced by an in-place reversal, 200,000 stores a round). Nothing else
// is changed. The headline rows are measured with ../../Main.java.
//
// Origin as ../../Main.java: ported from Valk's examples/bench/gc/main.valk
// (https://github.com/valk-lang/valk, tag 0.8.5, commit 33224300; MIT,
// (c) 2024 Lorenz Vandevelde; the license is in ../../upstream/LICENSE).

final class Node {
    Node next;
}

final class Tree {
    Tree left;
    Tree right;

    Tree() {
    }

    Tree(Tree left, Tree right) {
        this.left = left;
        this.right = right;
    }

    static Tree build(int depth) {
        if (depth <= 0)
            return new Tree();
        return new Tree(build(depth - 1), build(depth - 1));
    }

    long check() {
        long result = 1;
        if (left != null)
            result += left.check();
        if (right != null)
            result += right.check();
        return result;
    }
}

public final class Main {
    // Valk's globals g_sink and g_keep.
    static Node sink;
    static Node keep;

    static Node heapNode() {
        var node = new Node();
        sink = node;
        return node;
    }

    static Node chain(int count) {
        var first = new Node();
        var previous = first;
        for (int i = 1; i < count; i++) {
            var node = new Node();
            previous.next = node;
            previous = node;
        }
        return first;
    }

    static int chainLength(Node node) {
        int count = 0;
        while (node != null) {
            count++;
            node = node.next;
        }
        return count;
    }

    static long microsecondsSince(long start) {
        return (System.nanoTime() - start) / 1000;
    }

    static void printTime(long microseconds) {
        System.out.println("time_us:  " + microseconds);
        System.out.println("time_ms:  " + microseconds / 1000);
    }

    static long memoryKB() {
        var runtime = Runtime.getRuntime();
        return (runtime.totalMemory() - runtime.freeMemory()) / 1024;
    }

    static void header(String name) {
        System.out.println();
        System.out.println("=== " + name + " ===");
    }

    static void collectN(int count) {
        for (int i = 0; i < count; i++)
            System.gc();
    }

    // 1) Objects that die at once: every allocation is real (stored into
    //    the sink), only the last stays live.
    static void benchShortLived() {
        header("short-lived alloc");
        final int amount = 10_000_000;
        var root = new Node();

        long start = System.nanoTime();
        for (int i = amount; i > 0; i--) {
            var node = heapNode();
            if (i == 1)
                root.next = node;
        }
        long us = microsecondsSince(start);
        collectN(2);

        System.out.println("objects:  " + amount);
        printTime(us);
        System.out.println("rate:     " + (long) amount * 1000 / (us + 1) + " allocs/ms");
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + chainLength(root));
    }

    // 2) Large stable live set; allocate one trigger object before each
    //    forced collection.
    static void benchStableCollect() {
        header("stable-heap forced collects");
        final int live = 500_000;
        final int collects = 500;
        var list = chain(live);
        System.gc();

        long start = System.nanoTime();
        for (int i = 0; i < collects; i++) {
            heapNode();
            System.gc();
        }
        long us = microsecondsSince(start);

        System.out.println("live:     " + live);
        System.out.println("collects: " + collects);
        printTime(us);
        System.out.println("rate:     " + (long) collects * 1_000_000 / (us + 1) + " collects/s");
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + (chainLength(list) == live ? "OK" : "FAIL"));
    }

    // 3) Build a long-lived chain.
    static void benchBuildChain() {
        header("build long-lived chain");
        final int amount = 5_000_000;

        long start = System.nanoTime();
        var list = chain(amount);
        long us = microsecondsSince(start);
        keep = list;

        System.out.println("objects:  " + amount);
        printTime(us);
        System.out.println("rate:     " + (long) amount * 1000 / (us + 1) + " allocs/ms");
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + (chainLength(list) == amount ? "OK" : "FAIL"));
    }

    // 4) Drop a large graph and reclaim.
    static void benchFreeChain() {
        header("free long-lived chain");
        var list = keep;
        if (list == null) {
            System.out.println("skipped: no chain from build step");
            return;
        }
        int amount = chainLength(list);
        int check = amount;

        long start = System.nanoTime();
        // Drop every root, this local included, before reclaiming.
        keep = null;
        list = null;
        collectN(4);
        long us = microsecondsSince(start);

        System.out.println("objects:  " + amount);
        printTime(us);
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + check);
    }

    // 5) Rotate a live list: the head goes to the tail, 50 times.
    static void benchMutateLinks() {
        header("mutate live links");
        final int live = 200_000;
        final int rounds = 50;
        var list = chain(live);
        System.gc();

        long start = System.nanoTime();
        for (int round = 0; round < rounds; round++) {
            var head = list;
            var second = head.next;
            if (second == null)
                break;
            head.next = null;
            var tail = second;
            while (tail.next != null)
                tail = tail.next;
            tail.next = head;
            list = second;
        }
        long us = microsecondsSince(start);
        System.gc();

        System.out.println("live:     " + live);
        System.out.println("rounds:   " + rounds);
        printTime(us);
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + (chainLength(list) == live ? "OK" : "FAIL"));
    }

    // 6) Allocate short-lived garbage while a large live set stays rooted.
    static void benchChurnWithLive() {
        header("short-lived churn with large live set");
        final int live = 500_000;
        final int churn = 5_000_000;
        var list = chain(live);
        System.gc();

        long start = System.nanoTime();
        for (int i = churn; i > 0; i--)
            heapNode();
        System.gc();
        long us = microsecondsSince(start);

        System.out.println("live:     " + live);
        System.out.println("churn:    " + churn);
        printTime(us);
        System.out.println("rate:     " + (long) churn * 1000 / (us + 1) + " allocs/ms");
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + (chainLength(list) == live ? "OK" : "FAIL"));
    }

    // 7) Build and discard trees.
    static void benchTreeChurn() {
        header("tree churn");
        final int depth = 14;
        final int trees = 400;
        int nodesPerTree = (1 << (depth + 1)) - 1;

        long start = System.nanoTime();
        long check = 0;
        for (int i = 0; i < trees; i++)
            check += Tree.build(depth).check();
        System.gc();
        long us = microsecondsSince(start);

        System.out.println("depth:    " + depth);
        System.out.println("trees:    " + trees);
        System.out.println("nodes:    ~" + trees * nodesPerTree);
        printTime(us);
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + check);
    }

    // 8) ADDED BY AVANT, not part of Valk's suite: reverse a live list in place.
    static void benchReverseLinks() {
        header("reverse live links");
        final int live = 200_000;
        final int rounds = 50;
        var list = chain(live);
        System.gc();

        long start = System.nanoTime();
        for (int round = 0; round < rounds; round++) {
            Node previous = null;
            var current = list;
            while (current != null) {
                var next = current.next;
                current.next = previous;
                previous = current;
                current = next;
            }
            list = previous;
        }
        long us = microsecondsSince(start);
        System.gc();

        System.out.println("live:     " + live);
        System.out.println("rounds:   " + rounds);
        System.out.println("stores:   " + live * rounds);
        printTime(us);
        System.out.println("mem_kb:   " + memoryKB());
        System.out.println("verify:   " + (chainLength(list) == live ? "OK" : "FAIL"));
    }

    static void runScenario(int which) {
        switch (which) {
            case 1 -> benchShortLived();
            case 2 -> benchStableCollect();
            case 3 -> benchBuildChain();
            case 4 -> benchFreeChain();
            case 5 -> benchMutateLinks();
            case 6 -> benchChurnWithLive();
            case 7 -> benchTreeChurn();
            case 8 -> benchReverseLinks();
            default -> {
            }
        }
    }

    public static void main(String[] args) {
        System.out.println("Java GC bench suite");
        System.out.println("mem_kb before: " + memoryKB());

        if (args.length < 1) {
            for (int which = 1; which <= 7; which++)
                runScenario(which);
        }
        for (var arg : args)
            runScenario(Integer.parseInt(arg));

        System.out.println();
        System.out.println("=== done ===");
        System.out.println("mem_kb after: " + memoryKB());
    }
}
