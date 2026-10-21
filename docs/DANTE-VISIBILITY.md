# 为什么 Dante Controller 看不到接收端 —— 从发送端的可见性反推

本文用"发送端为什么可见"这条已知事实，推导"Dante Controller 会不会显示一个 AES67 接收端"。
结论：**不会**，而且这不是实现缺陷。

## 一、发送端的可见性条件（已知事实）

OpenAES67 的发送流能在 Dante Controller 里作为一条蓝色 AES67 流出现。它做的事只有两件：

1. 周期性向 `239.255.255.255:9875` 发送 **SAP 通告**（RFC 2974），载荷是一份 SDP；
2. 退出时发送删除通告（三次）。

SDP 内容（`openaes67 sdp` 实际输出）：

```
v=0
o=- 0 1 IN IP4 192.168.1.10
s=OpenAES67 stream
t=0 0
a=sendonly
m=audio 5006 RTP/AVP 96
c=IN IP4 239.69.1.2/32
a=source-filter: incl IN IP4 239.69.1.2 192.168.1.10
a=rtpmap:96 L16/48000/4
a=ptime:1
a=ts-refclk:localmac=00-1D-C1-FF-FE-12-34-56
a=mediaclk:direct=0
a=sync-time:0
a=framecount:48
a=mid:1
```

Audinate 的 `Creating AES67 or ST 2110-30 (RTP) Transmit Flows` 写得很直接：

> RTP transmit flows (including AES67 flows) from non-Dante devices appear in blue at
> the far right of the Routing tab … AES67 flows are identified by their transmit
> multicast IP addresses, rather than device names.

**即：DC 的这张表里，第三方的东西只有"流"，没有"设备"。流以组播地址为身份。**

## 二、关键证据：方向被忽略

在修复这个问题之前，OpenAES67 的发送端 SDP 里写的是 `a=recvonly` —— 方向标反了。
而 Dante Controller **照样把它显示为一条可订阅的发送流**。

由此可以确定：

> **Dante Controller 判定"是否显示"的依据是"存在一条 SAP 通告"，而不是通告里声明的方向。**

这条结论正好回答了"接收端能不能显示"：**不能靠方向字段区分，因为 DC 根本不看方向。**

## 三、推导

设 DC 的显示集合为 `S`。

1. 由第一节：`S` 至少包含 `{Dante 设备}` 与 `{SAP 通告的会话（渲染为发送流）}`。
2. 由第二节：SAP 会话无论标 `sendonly` 还是 `recvonly`，都只被当作"流"渲染，不会变成设备。
3. AES67 接收端的两个属性：它不是一个 Dante 设备（没有 Audinate 的发现身份），也不拥有会话
   （它只是订阅别人的组播地址；SAP 是发送者的工具）。
4. 因此接收端不属于 `S` 的任何一类 —— **它不可见，且没有任何 AES67 侧的手段能让它可见。**

### 推论 1：给接收端发一条 recvonly 通告会怎样？

会在 DC 里多出一条**蓝色流**，而不是一个接收设备。而且那条流指向的地址通常就是
Dante 设备自己已经在发的那条 —— 结果是同一地址出现两条条目，误导操作者。
**所以不该这么做。**

### 推论 2：要真正"以设备身份上线"需要什么？

需要让 DC 的发现流程把我们当成 Dante 设备：

- mDNS 服务通告（`_netaudio-arc._udp` / `_netaudio-cmc._udp` 一类）
- Audinate 的控制协议（CMC）会话，用来报告 TX/RX 通道数、接受订阅指令
- 设备证书/授权

这是 Audinate 的私有协议与授权范围。**Dante Virtual Soundcard 正是走这条路**（它是授权设备）。
只实现 mDNS 通告、不实现控制协议的结果更糟：DC 会列出一个设备，然后每次点它都失败。

## 四、实际方案：把"看不见"变成"不需要看见"

既然 DC 永远只显示流，就把接收侧做成**不依赖 DC 显示**也能用：

| 需求 | 现有实现 |
|---|---|
| 知道对方流在哪里 | SAP 自动发现（`sap-listen`、GUI 列表） |
| Dante 的 AES67 流不走 SAP | GUI「按地址接收」粘贴地址 |
| 不知道那条流的格式 | `probe`：听 2 秒反推通道/编码/ptime |
| 重启后还要重新配 | 地址与格式持久化；开机自启 |
| 出问题不知道卡在哪 | `doctor`：网卡/组播/SAP/ASIO/PTP 五项 |

## 五、一个必须遵守的约束

因为**流以组播地址为身份**，上行地址必须满足：

- 在 `239.x/16` 内（Brooklyn II、Dante HC、Broadway 等只订阅这个范围）；
- 与网络里已有流不冲突（DC 用地址区分条目，重复地址会互相覆盖）。

默认 `239.69.1.2:5006` 符合第一条。

## 六、什么情况下结论会改变

1. Audinate 在 Dante Controller 里加入"接收端条目"（目前没有；官方文档只描述流）；
2. Audinate 提供第三方设备接入的授权 SDK / DDM 代理注册机制；
3. 不再用 Dante Controller 作为视图，改用支持 AES67 双向视图的控制器
   （如支持 SAP 双向显示的分析工具）。

在此之前，任何"让接收端出现在 Dante Controller 里"的实现都只能靠伪装 Dante 设备，
代价是功能不可用 + 授权风险。
