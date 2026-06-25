# LRC Encoding and Placement Diagram for Papers

## Figure 1: LRC Encoding and Placement Overview

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                        LRC(F+1, 2, 2N-F-2) Encoding & Placement              │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│   ┌──────────────────┐                                                          │
│   │  Original Data   │                                                          │
│   │    (Message)     │                                                          │
│   └────────┬─────────┘                                                          │
│            │                                                                    │
│            ▼                                                                    │
│   ┌──────────────────────────────────────────────────────────────────┐         │
│   │                      Erasure Coding (LRC)                        │         │
│   │                                                                    │         │
│   │   k=4 Data Shards     l=2 Local Parities      r=8 Global Parities│         │
│   │   ┌────┬────┬────┬────┐    ┌────────┬────────┐   ┌─────────┐    │         │
│   │   │ d₀ │ d₁ │ d₂ │ d₃ │    │   p₀   │   p₁   │   │ g₀...g₇ │    │         │
│   │   └──┬─┴─┬──┴─┬──┴─┬──┘    └───┬────┴───┬───┘   └────┬────┘    │         │
│   │      │   │    │    │            │        │           │         │         │
│   └──────│───│────│────│────────────│────────│───────────│─────────┘         │
│          │   │    │    │            │        │           │                   │
│          ▼   ▼    ▼    ▼            ▼        ▼           ▼                   │
│   ┌──────────────────────────────────────────────────────────────────┐         │
│   │                    Node Placement (N=7, 2 frags/node)            │         │
│   │                                                                   │         │
│   │   ┌────────────┬────────────────────────┬────────────────────┐  │         │
│   │   │  LRC Grp 0 │  Nodes: {0, 1, 2, 3}   │  Intra-latency: ↓ │  │         │
│   │   │  ┌───────┐ │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐          │  │         │
│   │   │  │d₀,d₁  │ │  │ d₂  │ │ d₃  │ │ p₀  │ │     │          │  │         │
│   │   │  │+g₀,g₁│ │  │ +g₂  │ │ +g₃  │ │ +g₄  │ │     │          │  │         │
│   │   │  │(Node 0)│ │  │(N 1) │ │(N 2) │ │(N 3) │ │     │          │  │         │
│   │   │  └───────┘ │  └─────┘ └─────┘ └─────┘ └─────┘          │  │         │
│   │   ├────────────┼────────────────────────┼────────────────────┤  │         │
│   │   │  LRC Grp 1 │  Nodes: {4, 5, 6}     │  Intra-latency: ↓ │  │         │
│   │   │  ┌───────┐ │  ┌─────┐ ┌─────┐ ┌─────┐                  │  │         │
│   │   │  │       │ │  │ p₁  │ │ g₅  │ │g₇   │                  │  │         │
│   │   │  │       │ │  │ +g₅  │ │ +g₆  │ │+g₇  │                  │  │         │
│   │   │  │       │ │  │(N 4) │ │(N 5) │ │(N 6)│                  │  │         │
│   │   │  └───────┘ │  └─────┘ └─────┘ └─────┘                  │  │         │
│   │   └────────────┴────────────────────────────────────────────┘  │         │
│   └──────────────────────────────────────────────────────────────────┘         │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

## Figure 2: Fragment ID Convention

```
┌────────────────────────────────────────────────────────────────────┐
│                    Fragment ID Convention                           │
├────────────────┬─────────────────┬─────────────────────────────────┤
│  Fragment ID   │      Type      │           Description           │
├────────────────┼─────────────────┼─────────────────────────────────┤
│   [0, k)       │      Data      │ k data fragments                │
│   k = 0..3     │   d₀, d₁, d₂, d₃ │ distributed across nodes     │
├────────────────┼─────────────────┼─────────────────────────────────┤
│   [k, k+l)     │  Local Parity  │ l local parity fragments        │
│   k = 4..5     │     p₀, p₁     │ pᵢ belongs to LRC Group i      │
├────────────────┼─────────────────┼─────────────────────────────────┤
│ [k+l, k+l+r)   │ Global Parity  │ r global parity fragments       │
│   k = 6..13    │  g₀, g₁...g₇  │ cycle distributed (i % N)      │
└────────────────┴─────────────────┴─────────────────────────────────┘
```

## Figure 3: Node Placement Table

```
┌──────────┬───────────────────────┬──────────────────────────────────────────┐
│   Node   │   Fragments           │              Description                  │
├──────────┼───────────────────────┼──────────────────────────────────────────┤
│   0      │   [0] d₀,  [6] g₀    │ data fragment + global parity            │
│   1      │   [1] d₁,  [7] g₁    │ data fragment + global parity            │
│   2      │   [2] d₂,  [8] g₂    │ data fragment + global parity            │
│   3      │   [3] d₃,  [9] g₃    │ data fragment + global parity            │
│   4      │   [4] p₀,  [10] g₄   │ local parity (Grp 0) + global parity     │
│   5      │   [5] p₁,  [11] g₅   │ local parity (Grp 1) + global parity     │
│   6      │   [12] g₆, [13] g₇   │ two global parities (filler)            │
├──────────┴───────────────────────┴──────────────────────────────────────────┤
│ NOTE: Each node stores exactly 2 fragments, total = 14 = 2 × N               │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Figure 4: Recovery Properties

```
┌─────────────────────────────────────────────────────────────────────┐
│                    LRC Recovery Properties                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. Local Recovery (Fast Path):                                     │
│     ┌─────────────────────────────────────────────────────────┐     │
│     │  LRC Group 0: {d₀, d₁} + p₀  →  recover missing shard  │     │
│     │  LRC Group 1: {d₂, d₃} + p₁  →  recover missing shard  │     │
│     │                                                             │     │
│     │  Requirement: Same group members + local parity           │     │
│     │  Latency: Low (same group, low inter-node latency)        │     │
│     └─────────────────────────────────────────────────────────┘     │
│                                                                     │
│  2. Global Recovery (Fallback Path):                               │
│     ┌─────────────────────────────────────────────────────────┐     │
│     │  {d₀, d₁, d₂, d₃} + gᵢ  →  recover any missing shard  │     │
│     │                                                             │     │
│     │  Requirement: Any k data shards + any global parity        │     │
│     │  Latency: Higher (may cross groups)                       │     │
│     └─────────────────────────────────────────────────────────┘     │
│                                                                     │
│  3. Fault Tolerance (N=7, F=⌊N/2⌋=3):                             │
│     ┌─────────────────────────────────────────────────────────┐     │
│     │  • Up to F=3 node failures tolerated                     │     │
│     │  • 1 node down → local recovery via pᵢ                   │     │
│     │  • 2-3 nodes down → global recovery via gᵢ              │     │
│     └─────────────────────────────────────────────────────────┘     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## Figure 5: Latency-Aware Grouping

```
┌─────────────────────────────────────────────────────────────────────┐
│               Latency-Aware LRC Group Construction                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Input: N×N Latency Matrix                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │         │  N0  │  N1  │  N2  │  N3  │  N4  │  N5  │  N6  │    │
│  │    N0   │   0  │  58  │  77  │ 289  │ 191  │ 258  │ 224  │    │
│  │    N1   │  58  │   0  │ 293  │  53  │ 106  │ 171  │ 144  │    │
│  │    N2   │  77  │ 293  │   0  │  38  │ 210  │  50  │ 209  │    │
│  │    N3   │ 289  │  53  │  38  │   0  │  98  │  58  │ 134  │    │
│  │    N4   │ 191  │ 106  │ 210  │  98  │   0  │ 133  │ 239  │    │
│  │    N5   │ 258  │ 171  │  50  │  58  │ 133  │   0  │ 134  │    │
│  │    N6   │ 224  │ 144  │ 209  │ 134  │ 239  │ 134  │   0  │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Algorithm: Multi-Anchor Greedy Clustering                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  Phase 1: Select l=2 orthogonal anchors                    │    │
│  │    • Anchor 0: (1,2) with latency=293ms (max pair)          │    │
│  │    • Anchor 1: (3,5) with latency=58ms                      │    │
│  │                                                             │    │
│  │  Phase 2: Round-robin nearest assignment                     │    │
│  │    • Iteratively assign remaining nodes to nearest anchor    │    │
│  │                                                             │    │
│  │  Phase 3: Swap optimization (10 iterations)                  │    │
│  │    • Swap nodes between groups to reduce max intra-group     │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Result:                                                            │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  LRC Group 0: {1, 2, 3, 5}   avg_intra=49.8ms              │    │
│  │  LRC Group 1: {0, 4, 6}      avg_intra=218.0ms             │    │
│  │                                                             │    │
│  │  Goal: Minimize inter-group latency for local recovery      │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## Figure 6: System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                          Multi-Raft with Latency-Aware LRC                     │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│   ┌───────────────────────────────────────────────────────────────────────┐    │
│   │                         Physical Node Nᵢ                               │    │
│   │                                                                       │    │
│   │   ┌────────────────────────────────────────────────────────────────┐  │    │
│   │   │                     RaftStore                                  │  │    │
│   │   │                                                                │  │    │
│   │   │   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │  │    │
│   │   │   │  RaftNode G0 │  │  RaftNode G1 │  │  RaftNode Gk │      │  │    │
│   │   │   │  (Leader)    │  │  (Follower)  │  │  (Follower)  │      │  │    │
│   │   │   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │  │    │
│   │   │          │                 │                 │               │  │    │
│   │   │          └────────────┬────┴────────────────┘               │  │    │
│   │   │                       │                                     │  │    │
│   │   │                       ▼                                     │  │    │
│   │   │              ┌──────────────────┐                           │  │    │
│   │   │              │  LrcComplement- │                           │  │    │
│   │   │              │    aryGrouper   │◄── Latency Matrix       │  │    │
│   │   │              │                 │                         │  │    │
│   │   │              │  Groups: [G0,G1]│                         │  │    │
│   │   │              │  node_to_frags_ │                         │  │    │
│   │   │              └────────┬────────┘                           │  │    │
│   │   │                       │                                     │  │    │
│   │   └───────────────────────┼─────────────────────────────────────┘  │    │
│   │                           │                                       │    │
│   │                           ▼                                       │    │
│   │   ┌────────────────────────────────────────────────────────────────┐│    │
│   │   │                    Network Layer                              ││    │
│   │   │   • UnifiedRaftRpcServer (raft RPC)                         ││    │
│   │   │   • BatchTransportManager (fragment routing)                 ││    │
│   │   │   • Connection Pool (to other physical nodes)               ││    │
│   │   └────────────────────────────────────────────────────────────────┘│    │
│   │                                                                       │    │
│   └───────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
│   ┌───────────────────────────────────────────────────────────────────────┐    │
│   │                    Cross-Node Communication                          │    │
│   │                                                                       │    │
│   │   Node 0 ◄────────────────────────► Node 1 ◄────────────────► Node 2  │    │
│   │     │         Raft Consensus         │         Raft Consensus          │    │
│   │     │                                │                                │    │
│   │     │    ◄── AppendEntries ──►       │    ◄── AppendEntries ──►       │    │
│   │     │    (with LRC fragments)        │    (with LRC fragments)       │    │
│   │     │                                │                                │    │
│   │     └────────────────────────────────┴────────────────────────────────┘    │
│   │                      LRC Group 0 (low latency)                           │
│   │                                                                       │    │
│   └───────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

## Complete LaTeX Figure (for Paper)

```latex
\begin{figure}[t]
  \centering
  \caption{LRC Encoding and Latency-Aware Placement}
  \label{fig:lrc-placement}

  \begin{minipage}{\textwidth}
    \centering

    % Encoding Process
    \begin{tabular}{c}
      \toprule
      \textbf{Encoding} \\
      \midrule
      Original Data $\rightarrow$ \textbf{Erasure Coding} \\
      $\downarrow$ \\
      \begin{tabular}{ccc}
        k=4 Data & l=2 Local Parity & r=8 Global Parity \\
        $\{d_0,d_1,d_2,d_3\}$ & $\{p_0,p_1\}$ & $\{g_0,...,g_7\}$
      \end{tabular} \\
      \bottomrule
    \end{tabular}

    \vspace{1em}

    % Placement Table
    \begin{tabular}{|c|c|c|}
      \hline
      \textbf{Node} & \textbf{Fragments} & \textbf{Type} \\
      \hline
      0 & $[0]d_0, [6]g_0$ & Data + Global \\
      1 & $[1]d_1, [7]g_1$ & Data + Global \\
      2 & $[2]d_2, [8]g_2$ & Data + Global \\
      3 & $[3]d_3, [9]g_3$ & Data + Global \\
      4 & $[4]p_0, [10]g_4$ & Local + Global \\
      5 & $[5]p_1, [11]g_5$ & Local + Global \\
      6 & $[12]g_6, [13]g_7$ & Global + Global \\
      \hline
    \end{tabular}

    \vspace{1em}

    % Frag ID Convention
    \begin{tabular}{|c|c|l|}
      \hline
      \textbf{ID Range} & \textbf{Type} & \textbf{Description} \\
      \hline
      $[0,k)$ & Data & $k=4$ data fragments \\
      $[k,k+l)$ & Local Parity & $p_i$ belongs to LRC Group $i$ \\
      $[k+l,k+l+r)$ & Global Parity & Cycle distributed \\
      \hline
    \end{tabular}

  \end{minipage}

  \vspace{1em}

  \textbf{(b) Recovery Properties}
  \begin{itemize}
    \item \textbf{Local Recovery}: $\{d_i,d_j\} + p_k \rightarrow$ recover (same group)
    \item \textbf{Global Recovery}: $\{d_0,d_1,d_2,d_3\} + g_k \rightarrow$ recover (any node)
    \item \textbf{Fault Tolerance}: $F = \lfloor N/2 \rfloor = 3$ node failures
  \end{itemize}

\end{figure}
```
