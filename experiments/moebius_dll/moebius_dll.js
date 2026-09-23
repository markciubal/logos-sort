// moebius_dll.js — the "Moebius twist pointer routing" sort, as proposed (an outside conversation, 2026-09-21), so that it can be
// measured. Array -> doubly linked list; runs found by walking the list; a DESCENDING run is read back through its .prev
// pointers instead of being reversed; each run is extracted into an array; the runs are merged pairwise until one is left.
//
// Edits to the proposed code: it is wrapped in a module, the function is named moebiusSort, and two comments are reworded. The
// algorithm and `merge` are unchanged. Nothing was tuned.
//
// What it is, in the vocabulary of the rest of this repository: a natural merge sort (run detection, descending runs taken
// backwards, bottom-up pairwise merges) with a linked list in front of it. Its cost is O(n log R) for R runs. Reading a descending
// run backwards is what the engine's twist does (engines/c, rdesc_t.rev); the JS engine's run merge reverses in place.
//
// Bench: node --expose-gc experiments/moebius_dll/bench.js [pairs] [n]
module.exports = (() => {
    class DLLNode {
        constructor(val) {
            this.val = val;
            this.next = null;
            this.prev = null;
        }
    }

    function arrayToDLL(arr) {
        if (arr.length === 0) return null;
        let head = new DLLNode(arr[0]);
        let curr = head;
        for (let i = 1; i < arr.length; i++) {
            let node = new DLLNode(arr[i]);
            curr.next = node;
            node.prev = curr;
            curr = node;
        }
        return head;
    }

    function moebiusSort(arr) {
        if (arr.length <= 1) return arr;

        let head = arrayToDLL(arr);
        let runs = [];
        let curr = head;

        // Pass 1: Tangle Condensation via Moebius Twist
        while (curr !== null) {
            let startNode = curr;
            let isDescending = false;

            // Determine the slope of the current run
            if (curr.next && curr.next.val < curr.val) {
                isDescending = true;
            }

            // Walk the list until the slope changes
            while (curr.next !== null) {
                if (isDescending && curr.next.val >= curr.val) break;
                if (!isDescending && curr.next.val < curr.val) break;
                curr = curr.next;
            }

            let endNode = curr;
            let nextStart = curr.next; // Save pointer to the next run

            // Disconnect the run from the main list
            endNode.next = null;
            if (nextStart) nextStart.prev = null;

            // MOEBIUS TWIST: Extract to an array.
            // If it's descending, we simply read it using the `.prev` pointers instead of `.next`.
            let extractedRun = [];
            if (isDescending) {
                let temp = endNode;
                while (temp !== null && temp !== startNode.prev) {
                    extractedRun.push(temp.val);
                    temp = temp.prev;
                }
            } else {
                let temp = startNode;
                while (temp !== null) {
                    extractedRun.push(temp.val);
                    temp = temp.next;
                }
            }

            runs.push(extractedRun);
            curr = nextStart;
        }

        // Pass 2: Standard Merge of the pre-sorted runs
        while (runs.length > 1) {
            let merged = [];
            for (let i = 0; i < runs.length; i += 2) {
                if (i + 1 < runs.length) {
                    merged.push(merge(runs[i], runs[i + 1]));
                } else {
                    merged.push(runs[i]);
                }
            }
            runs = merged;
        }

        return runs[0];
    }

    function merge(left, right) {
        let result = [];
        let i = 0, j = 0;
        while (i < left.length && j < right.length) {
            if (left[i] <= right[j]) result.push(left[i++]);
            else result.push(right[j++]);
        }
        while (i < left.length) result.push(left[i++]);
        while (j < right.length) result.push(right[j++]);
        return result;
    }

    return { moebiusSort, merge, arrayToDLL, DLLNode };
})();
