# LRC Encoding and Placement Diagram

**For paper figures, use the Mermaid diagrams in `lrc_placement_diagram.md` or the ASCII diagrams in `lrc_diagram_for_paper.md`.**

## Quick Reference

```
LRC Parameters (N=7):
  k = ⌊N/2⌋ + 1 = 4  (data fragments)
  l = 2                (local groups)
  r = 2N - k - l = 8  (global parity fragments)
  total = k + l + r = 14 = 2N  (2 frags per node)

Frag ID Convention:
  [0, k)       = [0,4)   → Data:     d₀, d₁, d₂, d₃
  [k, k+l)     = [4,6)   → Local:    p₀, p₁
  [k+l, k+l+r) = [6,14)  → Global:   g₀ ... g₇

Node Placement:
  Node 0: [0] d₀,  [6] g₀
  Node 1: [1] d₁,  [7] g₁
  Node 2: [2] d₂,  [8] g₂
  Node 3: [3] d₃,  [9] g₃
  Node 4: [4] p₀,  [10] g₄
  Node 5: [5] p₁,  [11] g₅
  Node 6: [12] g₆, [13] g₇
```

## Mermaid Diagram Source

### Encoding Overview

```mermaid
flowchart LR
    subgraph ENC["LRC Encoding"]
        DATA["Original Data"] -->|Erasure Coding| K["k=4 Data<br/>d₀,d₁,d₂,d₃"]
        DATA -->|Erasure Coding| LP["l=2 Local<br/>p₀,p₁"]
        DATA -->|Erasure Coding| GP["r=8 Global<br/>g₀...g₇"]
    end

    subgraph PLACE["Placement"]
        N0["Node 0: d₀,g₀"]
        N1["Node 1: d₁,g₁"]
        N2["Node 2: d₂,g₂"]
        N3["Node 3: d₃,g₃"]
        N4["Node 4: p₀,g₄"]
        N5["Node 5: p₁,g₅"]
        N6["Node 6: g₆,g₇"]
    end

    K --> N0 & N1 & N2 & N3
    LP --> N4 & N5
    GP -.->|"cycle"| N0 & N1 & N2 & N3 & N4 & N5 & N6
```

### Recovery Paths

```mermaid
flowchart TB
    subgraph Local["Local Recovery (Fast)"]
        L1["LRC Group 0: {d₀,d₁} + p₀ → recover"]
        L2["LRC Group 1: {d₂,d₃} + p₁ → recover"]
    end

    subgraph Global["Global Recovery (Fallback)"]
        G1["Any 3 data + gᵢ → recover"]
    end

    subgraph Tolerance["Fault Tolerance"]
        F1["F = ⌊N/2⌋ = 3 failures"]
        F2["1 node down → local recovery"]
        F3["2-3 nodes down → global recovery"]
    end
```
