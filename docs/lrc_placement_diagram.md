# LRC Encoding and Placement Diagram

## Architecture Overview

```mermaid
flowchart TB
    subgraph ENC["Encoding Process"]
        D["Original Data"]
        D -->|Fragment| E1["Erasure Coding"]
        E1 -->|"k=4 Data Shards"| D1["d₀"]
        E1 -->|"k=4 Data Shards"| D2["d₁"]
        E1 -->|"k=4 Data Shards"| D3["d₂"]
        E1 -->|"k=4 Data Shards"| D4["d₃"]
        E1 -->|"Local Parity<br/>l=2"| LP1["p₀"]
        E1 -->|"Local Parity<br/>l=2"| LP2["p₁"]
        E1 -->|"Global Parity<br/>r=8"| GP1["g₀"]
        E1 -->|"..."| GPmid["..."]
        E1 -->|"Global Parity<br/>r=8"| GP8["g₇"]
    end

    subgraph FRAG["Fragment ID Mapping"]
        F0["[0] d₀<br/>Data"]
        F1["[1] d₁<br/>Data"]
        F2["[2] d₂<br/>Data"]
        F3["[3] d₃<br/>Data"]
        F4["[4] p₀<br/>Local Parity"]
        F5["[5] p₁<br/>Local Parity"]
        F6["[6] g₀<br/>Global Parity"]
        F13["[13] g₇<br/>Global Parity"]
    end

    subgraph GROUP["LRC Group Structure"]
        subgraph G0["LRC Group 0"]
            N0_0["Node 0"]
            N0_1["Node 1"]
            N0_2["Node 2"]
            N0_3["Node 3"]
        end
        subgraph G1["LRC Group 1"]
            N1_0["Node 4"]
            N1_1["Node 5"]
            N1_2["Node 6"]
        end
    end

    subgraph PLACEMENT["Node Placement (N=7)"]
        P0["Node 0<br/>[0, 6]"]
        P1["Node 1<br/>[1, 7]"]
        P2["Node 2<br/>[2, 8]"]
        P3["Node 3<br/>[3, 9]"]
        P4["Node 4<br/>[4, 10]"]
        P5["Node 5<br/>[5, 11]"]
        P6["Node 6<br/>[6, 12]"]
    end

    ENC --> FRAG
    FRAG --> PLACEMENT
```

## Detailed Placement Table

```mermaid
graph LR
    subgraph "LRC(4, 2, 8) - N=7 Nodes"
        subgraph Table["Placement Matrix"]
            T1["Node 0"]
            T2["Node 1"]
            T3["Node 2"]
            T4["Node 3"]
            T5["Node 4"]
            T6["Node 5"]
            T7["Node 6"]
        end

        subgraph Frags["Fragments"]
            F0["d₀ [0]"]
            F1["d₁ [1]"]
            F2["d₂ [2]"]
            F3["d₃ [3]"]
            F4["p₀ [4]"]
            F5["p₁ [5]"]
            F6["g₀ [6]"]
            F7["g₁ [7]"]
            F8["g₂ [8]"]
            F9["g₃ [9]"]
            F10["g₄ [10]"]
            F11["g₅ [11]"]
            F12["g₆ [12]"]
            F13["g₇ [13]"]
        end
    end

    T1 --- F0 & F6
    T2 --- F1 & F7
    T3 --- F2 & F8
    T4 --- F3 & F9
    T5 --- F4 & F10
    T6 --- F5 & F11
    T7 --- F6 & F12
```

## Frag ID Convention

```
┌─────────────────────────────────────────────────────────────────┐
│                    Fragment ID Convention                         │
├─────────────┬─────────────┬──────────────────────────────────────┤
│   Range     │    Type     │           Description               │
├─────────────┼─────────────┼──────────────────────────────────────┤
│  [0, k)     │   Data      │  k data fragments                  │
│             │             │  k = 4 for LRC(4,2,8)              │
├─────────────┼─────────────┼──────────────────────────────────────┤
│  [k, k+l)   │ Local Parity│  l local parity fragments           │
│             │             │  p₀ belongs to Group 0              │
│             │             │  p₁ belongs to Group 1              │
├─────────────┼─────────────┼──────────────────────────────────────┤
│ [k+l, k+l+r)│ Global Parity│ r global parity fragments          │
│             │             │  distributed evenly                  │
└─────────────┴─────────────┴──────────────────────────────────────┘
```

## Recovery Properties

```mermaid
graph TB
    subgraph LocalRecovery["Local Recovery (Any Group)"]
        L1["└─ Any 2 data shards in Group 0 + p₀ → recover"]
        L2["└─ Any 2 data shards in Group 1 + p₁ → recover"]
    end

    subgraph GlobalRecovery["Global Recovery"]
        G1["└─ Any 4 data shards + any gⱼ → recover"]
    end

    subgraph FailureScenarios["Failure Scenarios (N=7, F=3)"]
        FS1["└─ Up to F=3 node failures tolerated"]
        FS2["└─ 1 node down: local recovery via pᵢ"]
        FS3["└─ 2-3 nodes down: global recovery via gⱼ"]
    end
```

## Complete System Diagram

```mermaid
flowchart TB
    subgraph Input["Original Data Stream"]
        MSG["Message/Entry"]
    end

    subgraph Encoding["LRC Encoding"]
        EC["Erasure Coding"]
        EC -->|"k=4"| DATA["Data Shards<br/>d₀, d₁, d₂, d₃"]
        EC -->|"l=2"| LP["Local Parities<br/>p₀, p₁"]
        EC -->|"r=8"| GP["Global Parities<br/>g₀...g₇"]
    end

    subgraph Grouping["Latency-Based Grouping"]
        G0["LRC Group 0<br/>Nodes: {0,1,2,3}<br/>Intra-latency: low"]
        G1["LRC Group 1<br/>Nodes: {4,5,6}<br/>Intra-latency: low"]
    end

    subgraph Placement["Placement (2 frags/node)"]
        N0["Node 0: {d₀, g₀}"]
        N1["Node 1: {d₁, g₁}"]
        N2["Node 2: {d₂, g₂}"]
        N3["Node 3: {d₃, g₃}"]
        N4["Node 4: {p₀, g₄}"]
        N5["Node 5: {p₁, g₅}"]
        N6["Node 6: {g₆, g₇}"]
    end

    subgraph Recovery["Recovery Paths"]
        R1["Local: {d₀,d₁} + p₀ → Group 0"]
        R2["Local: {d₂,d₃} + p₁ → Group 1"]
        R3["Global: {d₀,d₁,d₂,d₃} + gᵢ → Any node"]
    end

    MSG --> EC
    DATA --> G0
    LP --> G1
    G0 --> N0 & N1 & N2 & N3
    G1 --> N4 & N5 & N6
    GP -.->|"cycle distribute"| N0 & N1 & N2 & N3 & N4 & N5 & N6
    N0 & N1 & N2 & N3 --> R1
    N4 & N5 & N6 --> R2
    N0 & N1 & N2 & N3 & N4 & N5 & N6 --> R3
```
