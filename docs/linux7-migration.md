# Linux 7 移植

> 執行人: Yu-Ting Wu (YTing46)
> 完整程式碼與工程筆記: [duo256m-mainline-amp](https://github.com/YTing46/duo256m-mainline-amp)

## 任務簡介

復刻並延伸 [HenryChaing 的「Linux 核心專題: 異質多核通訊機制」](https://hackmd.io/@sysprog/linux2025-projects)：在 Milk-V Duo 256M（SG2002，雙 T-Head C906 AMP 架構）上建立 Linux 大核與 ThreadX 小核的 RPMsg 通訊，並量化延遲。

原專題跑在 vendor 的 Linux 5.10 SDK 上；本專題把整條堆疊搬上 **mainline Linux 7.0-rc6**，過程中修整三個通訊 bug、發現並修正**四個 mainline 級的 bug**，最終達成 RPMsg 堆疊 **100% upstream 架構**——而且效能反超原本的手工 vendor glue。

## TODO 與完成狀態

- [x] mainline 7.0-rc6 在 Duo256M 開機（SD 卡 FIT image、免序列線的 adb 工作流）
- [x] RPMsg host↔C906L 穩定通訊（修掉 3 個移植 bug）
- [x] RTT 延遲分解量測體系 + payload sweep，對照 vendor 基線
- [x] upstream C906L remoteproc driver（Junhui Liu v2 系列）真機打通 + 3 個 review 修正
- [x] SEC_SYS clock 根因（mainline 級）+ 正式修法
- [x] RPMsg 全 upstream 化：remoteproc vdev + 標準 virtio_rpmsg_bus + mainline cv1800-mailbox
- [x] native 路徑 RTT 差距分析（hrtimer 風暴根因 + hard-IRQ 直送 → 反超 glue）
- [x] wake 段細部拆解、p99 尾巴來源定位
- [ ] throughput 與有負載尾延遲量測

## 系統架構

最終形態（與 vendor SDK 的對照）：

| 層級 | vendor 5.10（Henry） | 本專題 mainline 7.0（終態） |
|---|---|---|
| userspace | `/dev/rpmsg0`（ioctl） | 相同（不動） |
| rpmsg bus | `virtio_rpmsg_bus` | `virtio_rpmsg_bus`（標準） |
| virtio 接線 | `cvi_rpmsg_shmem.c`（vendor） | remoteproc vdev ← 韌體 resource table |
| kick / mailbox | `rtos_cmdqu.c`（vendor） | `cv1800-mailbox.c`（mainline，首個 in-tree 使用者） |
| 小核生命週期 | vendor rproc driver | `sophgo_cv1800b_c906l.c`（upstream 候選 + `.kick`） |
| 小核韌體 | FreeRTOS | ThreadX + rpmsg-lite + resource table |

kernel patch 系列（v7.0-rc6 起 24 個 patch，可完整重現）與韌體修改（7 個 patch）
都在 [GitHub repo](https://github.com/YTing46/duo256m-mainline-amp) 的 `patches/`。

## 量化結果

### RTT（pingpong 往返延遲，p50）

| 平台 | 4B | 480–496B | payload 斜率 |
|---|---|---|---|
| vendor 5.10（Henry 基線） | 114.0 µs | 131.8 µs | +17.9 µs |
| mainline 7.0 glue 優化版（cached buffers、RT kthread） | 152.0 µs | 162.1 µs | +10.1 µs |
| mainline 7.0 全 upstream（hrtimer 風暴修正前） | 176.4 µs | — | — |
| **mainline 7.0 全 upstream（終態：hrtimer 修正 + hard-IRQ 直送）** | **127.6 µs** | 139.2 µs | +11.6 µs |

觀察：

- 全 upstream 架構最初比手工 glue 慢 24µs，逐段分解後定位出兩個**可修的框架成本**
  （見下方發現 4），修正後 **RTT 反超手工 glue 逾 20µs**，距 vendor 原生只剩 13.6µs；
- 大 payload 斜率仍優於 vendor（+11.6 vs +17.9 µs）——virtio buffer pool
  從 uncached 改成 cached + 顯式 streaming DMA sync 的效果；
- 剩餘與 vendor 的差距逐段有主：使用者行程喚醒 ~28.6、deliver ~22.8、
  transport ~26.9、sysin ~20.4 µs——沒有未解之謎；
- RTT 有 boot-to-boot 變異（127–131µs），量級在量測誤差內。

### 穩定性

pingpong 300/300、1000/1000、熱重啟（rproc stop/start）後 vring 重同步正常、
開機自動流程（init script）全綠。

## 四個 mainline 級的發現

四個都是「vendor 世界不會發生、一到 mainline 規則下就爆炸」的問題，皆附最小重現與已驗證的修法。

### 1. SEC_SYS 暫存器區的隱藏時脈依賴

狀況：upstream remoteproc driver 一掛上，第一次 `echo start` 之後全板
I/O 凍死、adb 消失，看起來像 driver bug 或檔案系統問題。

原因：SEC_SYS 區塊（0x020B0000，小核 enable/bootaddr 所在）的匯流排存取
需要 `clk_efuse`。mainline 的 `clk_disable_unused()` 在開機尾聲把沒人認領的
時脈全關——之後碰這個區塊都會懸死 AXI interconnect（連 MMC 陪葬）。
最小重現：

```
# devmem 0x020B0004        ← 純讀，整板懸死
```

vendor 知道這條依賴，但把它藏在 C code 裡（crypto driver 每次存取前手動
`clk_prepare_enable(clk_efuse)`），device tree 隻字未提——「硬體使用說明
沒寫進公開合約」的典型。

修整：sec_sys syscon 節點宣告 `clocks = <&clk CLK_EFUSE>`，
syscon core 把時脈掛上 regmap，每次暫存器存取 just-in-time 開關。
driver 零改動，並補了專屬 DT binding 把 clocks 列為必要屬性。

### 2. cv1800-mailbox 的 enable 暫存器覆寫

mainline driver 的 ISR 用 `writeb(~valid, EN_REG)` 回應收訊——清掉目標 bit
的同時**把其他七個 bit 全拉高**。後果：對端以「自己的 enable bit 被清」當
tx-done 的 sender 永遠等不到、共享的 SET 暫存器會把自己送出的 kick 變成
自己的中斷。修法：正規 read-modify-write 只動一個 bit，另補 probe 時的
接收狀態重置（此硬體區塊不隨 CPU reset）。

### 3. remoteproc virtio 不繼承 DMA coherence（影響所有非同調平台）

`rproc_virtio_probe()` 給自動生成的 vdev 裝置繼承了 `dma_range_map`、
`dma_ops`、DMA mask——**唯獨漏了 `dma_coherent` 旗標**。這個裝置沒有
OF node，拿到架構預設（coherent）→ 在 dma-noncoherent 的 SoC 上，
vdev 配置的 ring/buffer 記憶體所有 `dma_sync_*()` **靜默變成 no-op**。

實測症狀：host 送出的 rpmsg 訊息在小核端讀到**全零 header**（資料卡在
大核的 cache）——在 rpmsg-lite 的分發點插探針記錄 `dst=0, src=0, len=0`
才定罪。修法一行（繼承旗標）。這個 bug 影響所有
「dma-noncoherent 平台 + remoteproc virtio」的組合。

### 4. mailbox polling 的 hrtimer 風暴

狀況：全 upstream 路徑比 glue 慢 24µs，且 p99 尾巴衝到 300µs。

原因：decomp 顯示 `mbox_send_message` 牆鐘 140µs（段位總和 324µs ≫ RTT，
代表有東西在偷牆鐘）。cv1800-mailbox 用 `txdone_poll` 但**沒設
`txpoll_period`（=0）**，framework 的 poll hrtimer 以
`hrtimer_forward_now(0)` **零週期在 hardirq context 連環觸發**，直到對端清
enable bit——單核的 C906B 整顆被這場中斷風暴偷走。

修法：kick 本來就冪等（兩端都「收到任一中斷就 poke 全部 virtqueue」），
改用 `knows_txdone`（TXDONE_BY_ACK）fire-and-forget——送完立即自報完成，
hrtimer 完全不啟動。RTT 176→164µs、p99 301→220µs。

延伸：wake 細拆進一步發現，mailbox 為 8-byte 訊息多付了一整個 threaded
IRQ 的排程來回（27.8µs），而下游全是 irq-safe 路徑（virtio 慣例本就在
hardirq 跑 vring callback）——把收訊改成 hard-IRQ 直送後 **RTT 127.6µs、
p99 178µs，反超手工 glue**。此改動需配套整條 rx 路徑的 irq-safety 改造
（mutex→spinlock、GFP_ATOMIC、NS 頻道建立延遲到 work item）。

## 移植過程修掉的三個通訊 bug（摘要）

| Bug | 根因 | 修法 |
|---|---|---|
| pingpong 數回合後 stall | vendor glue 用單一全域 cmd id + `schedule_delayed_work` 合併背靠背通知，mailbox ISR 只讀一次狀態 | 輪詢全部 virtqueue + ISR do/while 排空 |
| 時序性 1~數回合 stall | rpmsg 節點在 DT root 繼承不到 `dma-noncoherent`，virtio buffer 變 cacheable，無 cache 的小核讀到舊資料 | 節點補 `dma-noncoherent` |
| 開機樂透式 mailbox 全聾 | 小核的 mailbox 中斷是 edge-latch：IRQ init 清 pending 時線還高，之後永遠沒有新 edge | 先 quiesce（把線壓低）再 unmask |

每個 bug 的完整排查記錄（含被推翻的假設）見 repo 的
[工程筆記](https://github.com/YTing46/duo256m-mainline-amp/blob/main/docs/rpmsg-debug-note.md)。

## 全 upstream 化的五層根因鏈（除錯方法論案例）

把 RPMsg 從 vendor glue 換到 remoteproc vdev 架構時，pingpong 不通，一夜剝了五層洋蔥——每層都有鐵證、每層都不是最後一層：

1. **韌體 NS announce 無界 spin**：等一個被 host driver 的 EN 覆寫 quirk
   污染成永遠不清的 bit → spin 加上限。
2. **Bug #3 再現**：host 的 mailbox framework 排隊補送 kick，正好打進韌體
   quiesce 與 PLIC unmask 之間的窗口。
3. **scheduler 起跑前的中斷**（真擊殺者）：mainline host 在小核 reset 釋放後
   毫秒級就 kick（virtio probe）+ PLIC enable 跨 stop/start 存活 + ThreadX
   在 scheduler 起跑前開 `mie.MEIE` → 沒有 thread context 的狀態下進 ISR，
   排程器直接毀掉。修法：IRQ 註冊搬進 thread context。
   決定性實驗：**只改 resource table 的 section 名字**（host 認不得 → 不建
   vdev → 不 kick），韌體其他 byte 完全相同 → 開機完美，一刀切開
   「host 互動」與「韌體 image」。
4. **偵錯儀器自殺**：加的 9 參數 debug printf 在 1KB thread stack 上疊爆；
   同時發現 devmem 讀韌體全域變數經過**兩層 cache**（小核自身 D-cache +
   大核 /dev/mem 的 cached 視角），讀到的全是幻覺——可信探針要放
   uncached 共享位址 + 韌體顯式 cache clean。
5. **rproc_virtio dma_coherent**（上節發現 #3）→ 一行修正，300/300 全通。

方法論收穫：

- **觀測手段本身要先驗證**——雙核系統裡 devmem 不是 ground truth；
- **偵錯儀器是程式碼**，每輪只加最小儀器並先驗證儀器；
- **控制變因要「改一個 bit 換一個世界」**；
- **把處理搬進更嚴格的 context，整條下游的 sleeping 原語都要重新審計**
  ——hard-IRQ 直送後才發現 recv 路徑上還有兩把 mutex 與 GFP_KERNEL，
  「能跑」不等於「正確」；
- 症狀層 ≠ 兇手層：「I/O 全死」的根因可以在 clock framework，
  「訊息全零」的根因可以在 device model 的旗標繼承，
  「p99 尾巴」的根因可以只是 timer tick（HZ=250、131µs 視窗命中率 3.3%
  恰好對應 p97 起飛，經機率驗證非 bug）。

## 與 Henry 版的差異總結

| 面向 | Henry（vendor 5.10） | 本專題（mainline 7.0） |
|---|---|---|
| kernel | vendor SDK | mainline + 24 patches（可重現） |
| RPMsg 堆疊 | vendor glue | 100% upstream 架構 |
| RTT 4B p50 | 114 µs | 152 µs（glue 優化版）→ **127.6 µs（全 upstream 終態）** |
| payload 斜率 | +17.9 µs | +11.6 µs（cached buffers） |
| 額外產出 | — | 4 個 mainline bug 根因 + 修法、對停滯 rproc 系列的 review 與真機驗證 |

## 未竟事項

- throughput 與有負載尾延遲量測；
- binding 的 `dt_binding_check` / `dtbs_check` 正式驗證（需 dtschema 環境）。

## 心得

原以為異質多核的難處在高階的架構設計，真正投入後發現，關鍵往往藏在那些過去被忽略的底層細節裡——而且底層細節不只是坑，摸透之後反而成了突破口。

一開始只是想讓通訊跑起來，卻一路被三個「vendor 世界不會發生、一到 mainline 規則下就爆炸」的 bug 擋住，最戲劇性的是 upstream driver 一掛上就整板懸死，追了三層才發現真兇是 SEC_SYS 的匯流排時脈被 `clk_disable_unused` 關掉，一行 `devmem` 純讀就能重現。這讓我理解到，移植的本質是把 vendor 藏在 C code 裡的隱性硬體知識，一條條翻譯回 device tree 這份公開合約。

後半段把整條 RPMsg 換成 100% upstream 架構，並認真做效能分析。最初 native 比手工 glue 慢 24µs，我原本以為是「標準框架必然的架構稅」而打算認賠——但逐段分解後發現稅集中在兩個可修的點（poll hrtimer 空轉、多餘的 threaded IRQ 排程來回），修掉之後 **upstream 架構反而比手工實作快逾 20µs**，連 p99 尾巴都用機率驗證是 kernel timer tick 而非 bug。這推翻了我一開始的成見。

方法論上最深的體會是「觀測手段本身要先被驗證」：雙核系統裡 `devmem` 讀到的可能只是兩層 cache 之間的幻覺，我甚至一度被自己加的偵錯 printf 疊爆執行緒堆疊而誤判「韌體凍死」。偵錯儀器也是程式碼，每輪只加最小的量、而且要先確認儀器本身可信。

## 參考資料

- [HenryChaing — Linux 核心專題: 異質多核通訊機制](https://hackmd.io/@sysprog/linux2025-projects)
- [Junhui Liu — remoteproc: Add C906L controller for Sophgo CV1800B SoC (v2)](https://lore.kernel.org/linux-remoteproc/)
- [milkv-duo/duo-buildroot-sdk-v2 v2.0.1](https://github.com/milkv-duo/duo-buildroot-sdk-v2/releases/tag/v2.0.1)（vendor 參考基線）
- [本專題完整 repo：patches / 筆記 / 量測數據 / 板端腳本](https://github.com/YTing46/duo256m-mainline-amp)
