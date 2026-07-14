# Linux 核心專題: 異質多核通訊機制（mainline 移植篇）

> 執行人: Yu-Ting Wu (YTing46)
> 專題解說錄影（待補）
> 完整程式碼與工程筆記: [duo256m-mainline-amp](https://github.com/YTing46/duo256m-mainline-amp)

## 任務簡介

復刻並延伸 [HenryChaing 的「Linux 核心專題: 異質多核通訊機制」](https://hackmd.io/@sysprog/linux2025-projects)：在 Milk-V Duo 256M（SG2002，雙 T-Head C906 AMP 架構）上建立 Linux 大核與 ThreadX 小核的 RPMsg 通訊，並量化延遲。

原專題跑在 vendor 的 Linux 5.10 SDK 上；本專題把整條堆疊搬上 **mainline Linux 7.0-rc6**，過程中修掉三個通訊 bug、發現並修正**四個 mainline 級的 bug**，最終達成 RPMsg 堆疊 **100% upstream 架構**（vendor 專用 driver 全數退役）。

## TODO 與完成狀態

- [x] mainline 7.0-rc6 在 Duo256M 開機（SD 卡 FIT image、免序列線的 adb 工作流）
- [x] RPMsg host↔C906L 穩定通訊（修掉 3 個移植 bug）
- [x] RTT 延遲分解量測體系 + payload sweep，對照 vendor 基線
- [x] upstream C906L remoteproc driver（Junhui Liu v2 系列）真機打通 + 3 個 review 修正
- [x] SEC_SYS clock 根因（mainline 級）+ 正式修法
- [x] RPMsg 全 upstream 化：remoteproc vdev + 標準 virtio_rpmsg_bus + mainline cv1800-mailbox
- [x] native 路徑 RTT 差距分析（hrtimer 風暴根因 + 逐段歸屬）
- [ ] throughput 量測

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

kernel patch 系列（v7.0-rc6 起 21 個 patch，可完整重現）與韌體修改（5 個 patch）
都在 [GitHub repo](https://github.com/YTing46/duo256m-mainline-amp) 的 `patches/`。

## 量化結果

### RTT（pingpong 往返延遲，p50）

| 平台 | 4B | 496B | payload 斜率 |
|---|---|---|---|
| vendor 5.10（Henry 基線） | 114.0 µs | 131.8 µs | +17.9 µs |
| mainline 7.0 + 優化（cached buffers、RT kthread） | 152.0 µs | 162.1 µs | **+10.1 µs** |
| mainline 7.0 全 upstream 架構（修掉 hrtimer 風暴後） | 164.0 µs | 174.8 µs（480B） | +10.9 µs |

觀察：

- mainline 常數項落後 vendor ~30µs，分解量測顯示集中在 host 喚醒路徑
  （wake 段 ~62.7µs，近乎純常數）；
- **大 payload 斜率反超 vendor**（+10.1 vs +17.9 µs）——把 virtio buffer pool
  從 uncached 改成 cached + 顯式 streaming DMA sync 的效果；
- 全 upstream 架構相對 glue 多 12µs（8%）——decomp 逐段歸屬：vdev 分發間接層
  +8.6、韌體固定 slot 檢查 +3.3、mailbox framework 層疊 +2.0，
  是「標準框架 vs 手工 glue」的架構稅，每一微秒有主。

### 穩定性

pingpong 300/300、熱重啟（rproc stop/start）後 vring 重同步正常、
開機自動流程（init script）全綠。

## 四個 mainline 級的發現（本專題最有價值的部分）

四個都是「vendor 世界不會發生、一到 mainline 規則下就爆炸」的 bug，
皆附最小重現與已驗證的修法。

### 1. SEC_SYS 暫存器區的隱藏時脈依賴（整板懸死）

**症狀**：upstream remoteproc driver 一掛上，第一次 `echo start` 之後全板
I/O 凍死、adb 消失，看起來像 driver bug 或檔案系統問題。

**根因**：SEC_SYS 區塊（0x020B0000，小核 enable/bootaddr 所在）的匯流排存取
需要 `clk_efuse`。mainline 的 `clk_disable_unused()` 在開機尾聲把沒人認領的
時脈全關——之後**任何人**碰這個區塊都會懸死 AXI interconnect（連 MMC 陪葬）。
最小重現只要一行：

```
# devmem 0x020B0004        ← 純讀，整板懸死
```

vendor 知道這條依賴，但把它藏在 C code 裡（crypto driver 每次存取前手動
`clk_prepare_enable(clk_efuse)`），device tree 隻字未提——「硬體使用說明
沒寫進公開合約」的典型。

**修法**：sec_sys syscon 節點宣告 `clocks = <&clk CLK_EFUSE>`，
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

全 upstream 路徑最初比 glue 慢 24µs 且 p99 尾巴達 300µs。decomp 顯示
`mbox_send_message` 牆鐘 140µs——cv1800-mailbox 用 `txdone_poll` 但沒設
`txpoll_period`（=0），framework 的 poll hrtimer 以**零週期在 hardirq
context 連環觸發**直到對端清 enable bit，單核的 C906B 整顆被偷走。
修法：kick 本來就冪等（兩端都 poke 全部 virtqueue），改用
`knows_txdone`（TXDONE_BY_ACK）fire-and-forget——hrtimer 不再啟動，
RTT 176→164µs、p99 301→220µs。

## 移植過程修掉的三個通訊 bug（摘要）

| Bug | 根因 | 修法 |
|---|---|---|
| pingpong 數回合後 stall | vendor glue 用單一全域 cmd id + `schedule_delayed_work` 合併背靠背通知，mailbox ISR 只讀一次狀態 | 輪詢全部 virtqueue + ISR do/while 排空 |
| 時序性 1~數回合 stall | rpmsg 節點在 DT root 繼承不到 `dma-noncoherent`，virtio buffer 變 cacheable，無 cache 的小核讀到舊資料 | 節點補 `dma-noncoherent` |
| 開機樂透式 mailbox 全聾 | 小核的 mailbox 中斷是 edge-latch：IRQ init 清 pending 時線還高，之後永遠沒有新 edge | 先 quiesce（把線壓低）再 unmask |

每個 bug 的完整排查記錄（含被推翻的假設）見 repo 的
[工程筆記](https://github.com/YTing46/duo256m-mainline-amp/blob/main/docs/rpmsg-debug-note.md)。

## 全 upstream 化的五層根因鏈（除錯方法論案例）

把 RPMsg 從 vendor glue 換到 remoteproc vdev 架構時，pingpong 不通，
一夜剝了五層洋蔥——每層都有鐵證、每層都不是最後一層：

1. **韌體 NS announce 無界 spin**：等一個被 host driver 的 EN 覆寫 quirk
   污染成永遠不清的 bit → spin 加上限。
2. **Bug #3 重演**：host 的 mailbox framework 排隊補送 kick，正好打進韌體
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
- 症狀層 ≠ 兇手層：「I/O 全死」的根因可以在 clock framework，
  「訊息全零」的根因可以在 device model 的旗標繼承。

## 與 Henry 版的差異總結

| 面向 | Henry（vendor 5.10） | 本專題（mainline 7.0） |
|---|---|---|
| kernel | vendor SDK | mainline + 19 patches（可重現） |
| RPMsg 堆疊 | vendor glue | 100% upstream 架構 |
| RTT 4B p50 | 114 µs | 152 µs（glue 優化版）/ 164 µs（全 upstream） |
| payload 斜率 | +17.9 µs | +10.1 µs（cached buffers） |
| 額外產出 | — | 4 個 mainline bug 根因 + 修法、對停滯 rproc 系列的 review 與真機驗證 |

## 未竟事項

- throughput 與有負載尾延遲量測；
- wake 段（native 71.8µs）的細部拆解與縮減（vdev 分發間接層）。

## 參考資料

- [HenryChaing — Linux 核心專題: 異質多核通訊機制](https://hackmd.io/@sysprog/linux2025-projects)
- [Junhui Liu — remoteproc: Add C906L controller for Sophgo CV1800B SoC (v2)](https://lore.kernel.org/linux-remoteproc/)
- [milkv-duo/duo-buildroot-sdk-v2 v2.0.1](https://github.com/milkv-duo/duo-buildroot-sdk-v2/releases/tag/v2.0.1)（vendor 參考基線）
- [本專題完整 repo：patches / 筆記 / 量測數據 / 板端腳本](https://github.com/YTing46/duo256m-mainline-amp)
