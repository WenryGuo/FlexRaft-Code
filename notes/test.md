**要测试写延迟、吞吐、带宽、读延迟、故障恢复（leader故障恢复速度、带宽消耗）**
# flexraft在orange pi 5 plus上的性能：
## 3节点 RS(2,1)
测试记录：

**4K**:[Results][Succ Cnt=500][Average Latency = 23321 us][Average Commit Latency = 17636 us][Average Apply Latency = 91 us]

**16K**:[Results][Succ Cnt=500][Average Latency = 27933 us][Average Commit Latency = 21186 us][Average Apply Latency = 190 us]

**64K**：[Results][Succ Cnt=500][Average Latency = 33478 us][Average Commit Latency = 23660 us][Average Apply Latency = 555 us]

**128K**：[Results][Succ Cnt=500][Average Latency = 48715 us][Average Commit Latency = 33741 us][Average Apply Latency = 1023 us]

**256K**：[Results][Succ Cnt=500][Average Latency = 70761 us][Average Commit Latency = 47228 us][Average Apply Latency = 1828 us]

**512K**：[Results][Succ Cnt=500][Average Latency = 109365 us][Average Commit Latency = 67665 us][Average Apply Latency = 3627 us]

**1024K**：[Results][Succ Cnt=500][Average Latency = 164580 us][Average Commit Latency = 99117 us][Average Apply Latency = 7672 us]

**2048K**：[Results][Succ Cnt=500][Average Latency = 296271 us][Average Commit Latency = 160227 us][Average Apply Latency = 14622 us]

## 5节点RS(3,2)
测试记录：

**4K**:[Results][Succ Cnt=500][Average Latency = 36166 us][Average Commit Latency = 28419 us][Average Apply Latency = 90 us]

**16K**:[Results][Succ Cnt=500][Average Latency = 38748 us][Average Commit Latency = 30686 us][Average Apply Latency = 244 us]

**64K**：[Results][Succ Cnt=500][Average Latency = 59917 us][Average Commit Latency = 47733 us][Average Apply Latency = 575 us]

**128K**：[Results][Succ Cnt=500][Average Latency = 80365 us][Average Commit Latency = 62934 us][Average Apply Latency = 1041 us]

**256K**：[Results][Succ Cnt=500][Average Latency = 84627 us][Average Commit Latency = 64248 us][Average Apply Latency = 1832 us]

**512K**：[Results][Succ Cnt=500][Average Latency = 117476 us][Average Commit Latency = 83877 us][Average Apply Latency = 3641 us]

**1024K**：[Results][Succ Cnt=500][Average Latency = 203836 us][Average Commit Latency = 133305 us][Average Apply Latency = 7361 us]

**2048K**：[Results][Succ Cnt=500][Average Latency = 366551 us][Average Commit Latency = 206496 us][Average Apply Latency = 14276 us]


## 7节点RS(4,3)
测试记录：

**4K**:[Results][Succ Cnt=500][Average Latency = 28586 us][Average Commit Latency = 23098 us][Average Apply Latency = 88 us]

**16K**:[Results][Succ Cnt=500][Average Latency = 27180 us][Average Commit Latency = 21332 us][Average Apply Latency = 176 us]


**64K**：[Results][Succ Cnt=500][Average Latency = 39518 us][Average Commit Latency = 31090 us][Average Apply Latency = 498 us]

**128K**：[Results][Succ Cnt=500][Average Latency = 54778 us][Average Commit Latency = 42017 us][Average Apply Latency = 974 us]

**256K**：[Results][Succ Cnt=500][Average Latency = 85835 us][Average Commit Latency = 63839 us][Average Apply Latency = 1793 us]

**512K**：[Results][Succ Cnt=500][Average Latency = 133670 us][Average Commit Latency = 96938 us][Average Apply Latency = 3544 us]

**1024K**：[Results][Succ Cnt=500][Average Latency = 197508 us][Average Commit Latency = 132658 us][Average Apply Latency = 7109 us]

**2048K**：[Results][Succ Cnt=500][Average Latency = 380303 us][Average Commit Latency = 218614 us][Average Apply Latency = 14211 us]


## 9节点RS(5,4)
测试数据：

**4K**:[Results][Succ Cnt=500][Average Latency = 28586 us][Average Commit Latency = 23098 us][Average Apply Latency = 88 us]

**16K**:[Results][Succ Cnt=500][Average Latency = 27180 us][Average Commit Latency = 21332 us][Average Apply Latency = 176 us]


**64K**：[Results][Succ Cnt=500][Average Latency = 39518 us][Average Commit Latency = 31090 us][Average Apply Latency = 498 us]

**128K**：[Results][Succ Cnt=500][Average Latency = 54778 us][Average Commit Latency = 42017 us][Average Apply Latency = 974 us]

**256K**：[Results][Succ Cnt=500][Average Latency = 85835 us][Average Commit Latency = 63839 us][Average Apply Latency = 1793 us]

**512K**：[Results][Succ Cnt=500][Average Latency = 133670 us][Average Commit Latency = 96938 us][Average Apply Latency = 3544 us]

**1024K**：[Results][Succ Cnt=500][Average Latency = 197508 us][Average Commit Latency = 132658 us][Average Apply Latency = 7109 us]

**2048K**：[Results][Succ Cnt=500][Average Latency = 380303 us][Average Commit Latency = 218614 us][Average Apply Latency = 14211 us]

## 11节点RS(6,5)

# 为了对标LRC纠删码的集群配置，测试双数节点的性能
## 6节点
## 8节点
## 10节点

# 测试 Multi-Raft+flexraft的性能：