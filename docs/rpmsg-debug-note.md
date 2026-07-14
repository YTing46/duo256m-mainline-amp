# 在 mainline Linux 7.0 上讓 Milk-V Duo256M 雙核 RPMsg 通訊——除錯筆記（已結案）

> 對象平台：Milk-V Duo256M（SG2002，雙 C906：大核 C906B 跑 Linux、小核 C906L 跑 ThreadX）
> 目標：把 HenryChaing 的 NCKU 專題（ThreadX + RPMsg-lite + remoteproc + ADB）從原廠 Linux 5.10.4 SDK 移植到 **mainline Linux 7.0-rc6**
> 本筆記聚焦於「雙核 RPMsg 只通第一發就 stall」這個 bug 的排查歷程與根因定位。
> 日期：2026-07-06 起，2026-07-07 結案（三個 bug 皆修復驗證，pingpong 16/16；延遲 155→~133µs）。凡未經實測證實者一律標註「待驗證」。

## 0. 一句話結論

RPMsg 的接收（RX）與 echo 交付其實都會動。「pingpong 只回一次就卡死」其實是**兩個獨立問題疊在一起**：

- **Bug #1（已修復）**：測試工具 mailbox_test 的 ioctl enum 與核心對不上，導致它一執行就把 RPMsg 的中斷 handler 蓋掉，echo 全被丟棄。這同時也是我除錯時自己踩的坑——把 mailbox_test 當對照組，反而每次都打壞 RPMsg。
- **Bug #2（已修復）**：真根因是**一行 devicetree**——rpmsg 節點放在根層級、繼承不到 soc 節點的 `dma-noncoherent`，於是 Linux 把這個裝置當成 DMA 同調，`dma_alloc_coherent()` 配出來的 256KB 訊息緩衝區是**普通可快取記憶體、零快取維護**。大核（C906B）寫入的訊息內容卡在自己的 D-cache、要等隨機的 cache eviction 才落到 DRAM，無快取同調的小核（C906L）便讀到舊資料——造成 pingpong 以「時好時壞、1～4 輪」的姿態 stall。DTS 補上 `dma-noncoherent;` 後，pingpong 16/16 穩定重現（連跑 6 次 + 熱重啟全綠）。

到達這一行之前，繞了六版 kernel 的通知層修法與一整排被證偽的假設——過程完整記錄如下，因為**排除的路徑與方法比答案本身更值錢**。凡未證實者標「待驗證」。

---

## 1. 背景與既有進度

移植到 7.0-rc6 的開機問題先前已解（FIT 載入位址 0x82000000，避免 27.5MB 的 Image 解壓時覆蓋 FIT，另有筆記）。本階段起點：系統已能開機到 root shell，remoteproc 起得了 C906L（state=running），`/dev/rpmsg0`、`/dev/rpmsg_ctrl0`、`/dev/cvi-rtos-cmdqu` 都在，mailbox 中斷（PLIC 101）已接。

殘留問題：**RPMsg pingpong 只收到第一次 echo 就永久 stall**。這份筆記記錄如何把它從「以為 RX 全壞」一路釐清到真正的根因。

---

## 2. 環境與方法：兩個先擋路的基礎建設坑

在能觀察小核之前，先踩了兩個與 bug 無關、但不解決就寸步難行的坑。記下來供後人少走冤枉路。

### 2.1 序列埠 baud 不是 115200，而是 124000

C906L 的除錯輸出（前綴 `RT:`）與 Linux console 共用同一顆 UART0，`console=ttyS0,115200`。但用 PL2303 USB-TTL 以 115200 讀，整片亂碼。

從板子直接讀到 UART 參考時脈：

```
# cat /sys/kernel/debug/clk/clk_uart0/clk_rate
25000000
```

dw-apb-uart 的除數 = round(25000000 / (16 × 115200)) = round(13.56) = 14，實際線速 = 25000000 / (16 × 14) ≈ **111607**，與 115200 差約 3.1%。理論上 3% 在容忍範圍內，但這顆 PL2303 clone 的 baud 產生器非線性，實測要把 host 設成 **124000** 才能乾淨解碼（用 termios2 的 BOTHER 設非標準 baud；所有標準 baud 皆亂碼）。

判定方法：讓 C906L 連續輸出已知字串，host 端掃描各 baud，計算「可列印字元比例 + 命中已知字串次數」，124000 拿到 100% 可讀 + 8 次命中。設對之後，序列上同時看得到 Linux 的 root shell 提示字元 `[root@milkv-duo]~#` 與 C906L 的 `RT:` 輸出。

> 待驗證：124000 是這條特定 PL2303 的偏差值，換線可能不同；正解應是量測該 USB-TTL 對 115200 標稱值的實際偏移。

另註：`/dev/ttyUSB0` 每次 USB 重新列舉就把權限打回 root:dialout。一勞永逸解法是 udev 規則：

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="067b", ATTRS{idProduct}=="2303", MODE="0666"
```

### 2.2 `adb reboot` 在這個移植版是空操作

多次 `adb reboot` 後，Linux 的 `/proc/uptime` 連續不歸零、C906L 的 threadx tick 也從不重置。結論：**mainline SG2002 尚未接上 restart handler，reboot 是靜默 no-op**。過程中反覆出現的「reboot 後 adb 變 offline」其實只是 USB gadget 的握手掉了（需實體重插或 kill-server），不是真的重開機。

要真正重置狀態只有兩條路：

1. **實體斷電**（拔插 USB 電源）——唯一能連 C906L 一起冷重置的方法。
2. **remoteproc `echo stop; echo start`（從 running 狀態）**——assert/deassert RST_CPUSYS2，重啟小核並重載 `/lib/firmware/cvirtos.elf`，可拿到乾淨 channel，不必斷電。

> 修正一個先前的誤記：手動 restart 並非「壞掉」。之前失敗是因為用了 start;stop;start 的順序（already-running 時第一個 start 讓狀態機混亂）。正確順序是從 running 直接 stop 再 start，實測 state 走 running→offline→running，小核 threadx 計數器歸零、印出 create cvi task。

---

## 3. 排查歷程：一路排除，直到把 bug 逼到牆角

方法論：對每個假設設計「能證偽它」的最小實驗，量化證據，錯了就記錄下來翻案。

### 3.1 先分辨「小核死了」還是「只有 vring 壞」

觀察到兩條 Linux→C906L 的路徑：

- **mailbox 命令路徑**（rtos_cmdqu 的 CMD_DUO_LED）：資料放在 mailbox 暫存器（0x01900000，MMIO，天生 non-cacheable）。
- **rpmsg 路徑**（pingpong / led）：資料放在 vring 共享記憶體（0x8fc00000，可快取 DRAM）。

實驗：連續跑 5 次 CMD_DUO_LED，數 mailbox 中斷增量。

```
run 1: IRQ 3 -> 4 (delta 1)
run 2: IRQ 4 -> 5 (delta 1)
...
run 5: IRQ 7 -> 8 (delta 1)
```

**結論：mailbox 命令路徑每次都通、可重複。** 所以 C906L 活著、mailbox ISR（prvQueueISR）與命令派送（prvCmdQuRunTask）正常、enable-bit 交握正確。這一步同時驗收了 Task 3（rtos_cmdqu ioctl 路徑）。

對比之下 rpmsg 路徑（led_sweep）在同一時刻完全沒有中斷回應。問題被縮到 vring 這一層。

### 3.2 逐一排除四個看似合理的嫌疑

| 假設 | 如何證偽 | 結論 |
|---|---|---|
| C906L 核心死了 | mailbox 命令 5/5 有回應 | 排除 |
| mailbox ISR 壞了 | 同上 | 排除 |
| enable-bit 沒重新武裝（`cpu_mbox_en &= ~valid`） | Linux `SEND_TO_CPU=2` 與韌體 `RECEIVE_CPU=2` 同 index（`-DFREERTOS_BSP`），且命令路徑 5/5 通 | 排除 |
| rpmsg-lite 缺 cache invalidate | 反組譯 cvirtos.elf：`virtqueue_get_available_buffer` 確實呼叫 `env_cache_invalidate`，且它是 tail-call thunk → `platform_cache_invalidate` → `inv_dcache_range`（真的跑 `dcache.ipa`+`sync.s`）；`RL_USE_DCACHE=1` 生效 | 排除 |

> 誠實記錄一次翻案：`env_cache_invalidate` 的符號大小只有 2 bytes，一度被誤判為「空殼函式」。反組譯後發現那 2 bytes 是壓縮跳轉指令（tail-call thunk），cache invalidate 其實有實作且會執行。假設就此被推翻。

### 3.3 關鍵轉折：插探韌體證明「RX 其實是通的」

排除法走到這裡，headless（純 adb + Linux 端）的觀測已用盡，ftrace 在這顆 kernel 也沒編進可用的 function tracer。唯一能繼續的方法是**在韌體加 printf 直接看小核在幹嘛**。

在 comm_main.c 加了三處除錯輸出（都以 `RT: DBG` 為前綴）：

- `prvQueueISR` 進入時印 `set` 值（mailbox kick 有沒有觸發小核中斷）
- `prvCmdQuRunTask` 取出命令時印 cmd_id（有沒有派送、cmd_id 是多少）
- case 0/1 呼叫 `env_isr()` 前印一行（vring 通知有沒有進 rpmsg 處理）

重建 cvirtos.elf（CMake+Ninja、`DDR_64MB_SIZE=n`、需先產生 cvi_board_memmap.ld），推到 `/lib/firmware/cvirtos.elf`，**實體斷電重開**讓它乾淨載入（dmesg 確認載入大小 456264，正是插探版）。

在乾淨 channel 上跑 led_sweep（送 period 到 LED endpoint 0x20），序列吐出：

```
RT: DBG ISR set=0x00000001
RT: DBG task cmd_id=1 param=0x00010000 lv=1
RT: DBG env_isr(1)
period: 80
```

而且連續三發（period 設成 80 → 800 → 80）全部成功。**這推翻了先前「led callback 從不觸發、RX 全壞」的結論**——那個錯誤結論是在舊韌體 / 未斷電重置的髒狀態下量到的。RX 完整路徑其實是通的：kick 到達、env_isr 執行、callback 觸發、period 有被設定。

> 教訓：任何 rpmsg 觀測都必須在「實體斷電後的乾淨 channel」上做，否則會被前一次測試殘留的髒 vring 狀態誤導。

### 3.4 把 bug 逼到牆角：host 在第一發後停止 kick

既然 RX 會動，改回原始的 pingpong（一寫一讀共 16 次），在乾淨 channel 上讓它第一個跑：

```
RT: DBG ISR set=0x00000001
RT: DBG task cmd_id=1 param=0x00010000 lv=1
RT: DBG env_isr(1)
send: 1
```

第一發**完全成功**：小核收到、主 endpoint callback 觸發、thread 印出 send: 1、echo 回 host（mailbox IRQ +1）。

但**只有第一發**。pingpong 要跑 16 次，卻在第一次 read echo 之後就 stall。關鍵證據：插探計數在整個過程中都停在 1（ISR=1、task=1、env_isr=1）。也就是說——

**第二次 write 根本沒讓 C906L 的 ISR 觸發。** host 把 buffer 放進了 vring，卻沒有對小核發 mailbox 通知。

---

## 3.5 翻案：先前幾個推論被推翻

在往下之前，誠實記錄三個一度成立、後來被證偽的推論，以免誤導：

- **「host 送第一發後就不再 kick」——部分是假象。** 後來加了計數才發現：純寫入的 led_sweep 其實三發都有 kick，它「停在三發」只是我 timeout 給太短（每發間隔 sleep 3 秒，五發要十五秒，我只給八九秒）。真正 stall 的只有會讀 echo 的 pingpong。
- **「vring buffer 塞滿 → write 阻塞」——基於錯誤的緩衝區數量。** 我一度以為 `RPMSG_NUM_BUFS` 是 2（被原始碼註解 `/*2*/` 誤導），實際巨集是 **256**。所以不是緩衝區用罄。
- **「host 與 C906L 的 EVENT_IDX 通知抑制不一致」——排除。** host 端 `cvi_rpmsg_get_features` 只回報 `1<<0`（F_NS），沒有協商 EVENT_IDX，走的是讀 `used->flags` 的路徑（該值恆為 0）。

真正卡住 pingpong 第一發 echo 的，是下面的 Bug #1。

## 4. Bug #1：ioctl 命令號錯位，測試工具打壞 RPMsg（已修復）

用插探韌體確認 RX 會動之後，pingpong 卻仍在第一個 read（等 echo）就卡死；韌體明明送了 echo（序列印 `send: 1`、host mailbox 中斷 +1）。加上 host 端觀察，dmesg 出現決定性線索：

```
callback_rtos_hander cmd_id = 0 ptr=f
```

`ptr=0xf` 正是韌體 `mailbox_send` 帶的 `param_ptr`。也就是說 C906L→host 的 RPMsg echo 通知，被派到了 `callback_rtos_irq_handler`（rtos_cmdqu.c 裡一個只印一行、什麼都不做的除錯 handler），而不是 RPMsg 驅動的 `rtos_cmdqu_handler`（後者才會去處理 virtqueue、把 echo 交給 /dev/rpmsgN）。兩者都登記在 `rtos_irqaction[ip_id=0]`，後登記者覆蓋前者。

**是誰去覆蓋的？** 二分實驗：先用 `unbind`/`bind` cvi-rpmsg 驅動重跑 probe、重新登記 `rtos_cmdqu_handler`（解除覆蓋），pingpong 立刻讀得到 echo；接著單獨跑一次 mailbox_test，覆蓋又回來了。所以元兇是 mailbox_test。

**根因**：mailbox_test.c 自帶一份精簡的 ioctl 命令列舉，與核心的不一致——

| 命令號 | 核心 rtos_cmdqu.h | mailbox_test.c（修正前） |
|---|---|---|
| 1 | CMDQU_SEND | CMDQU_SEND |
| 2 | **CMDQU_REQUEST** | CMDQU_SEND_WAIT |
| 4 | CMDQU_SEND_WAIT | （無） |

於是 mailbox_test 呼叫 `RTOS_CMDQU_SEND_WAIT`（= `_IOW('r', 2)`）時，核心比對到的是 `RTOS_CMDQU_REQUEST`（同為 `_IOW('r', 2)`），跑進 `request_rtos_irq(ip_id=0, callback_rtos_irq_handler)` → 覆蓋掉 RPMsg 的 handler → 之後所有 C906L→host 的 RPMsg echo 靜默遺失 → pingpong 的 read 卡死。

**修法**：把 mailbox_test.c 的列舉補齊、與核心 ABI 對齊（插入 CMDQU_REQUEST / CMDQU_REQUEST_FREE）。**已驗證**：修正後再跑 mailbox_test，`callback_rtos_hander` 不再出現、RPMsg handler 不被覆蓋。

> 教訓：這個 bug 也一路在污染我的除錯——我把 mailbox_test 當「對照組」反覆執行，等於每次都親手把 RPMsg 打壞。除錯工具本身必須先驗證乾淨。

## 4b. Bug #2 排除之路：六版 kernel 與一排倒下的假設

修好 Bug #1 後，pingpong 仍只跑 1～數輪就 stall。以下是 2026-07-07 的深挖——最終答案在 §4c，但這一節記錄**怎麼把假設一個個殺掉**，包含對自己初判的多次翻案。

### 4b.1 翻案：不是韌體端，是 host 端

初判懷疑韌體端（`thread_rpmsg` 忙輪詢、`g_has_received` 是旗標非計數、echo 方向 enable-bit）。為了直接看小核在收發什麼，在韌體加了兩個插探：`rpmsg_lite_send` 的回傳值（`send: N ret=%d`）與接收 callback 進入點（`readcb DATA=%d src=%d`）。乾淨斷電後跑 pingpong，TTL 序列顯示：

```
env_isr(1) → readcb DATA=0 → send: 1 ret=0
env_isr(1) → readcb DATA=1 → send: 2 ret=0
env_isr(1) → readcb DATA=2 → send: 3 ret=0
...
```

**韌體端一輪輪完全正常**：每次都收到 host 的 write、echo 都 `ret=0`（RL_SUCCESS）送出。這**推翻了「韌體端問題」的初判**。同時排除了「TX buffer 耗盡」（ret 恆為 0）。

補一個實體證據：pingpong stall 當下，從 Linux 端用 `devmem` 直接讀 vring 的生產者／消費者索引（vring0 = remote→host，vring1 = host→remote）：

```
vring0 (echo) used.idx = 6     ← 韌體送了 6 個 echo 進 used ring
pingpong 實際收到           = 3     ← host 只交付了 3 個給 reader
```

**韌體送出的 echo 比 host 交付給使用者的多。** 問題確定在 host 收／派發側，不在韌體。

### 4b.2 排除一連串 host／快取假設（都證偽）

| 假設 | 如何證偽 | 結論 |
|---|---|---|
| C906 D-cache 硬體預取讓 avail->idx 讀到 stale | 關掉預取（mhint bit2 DPLD，`0x610c→0x6108`）重測，無改善 | 排除 |
| C906L 讀 vring buffer/ring 讀到 stale 快取 | 讀 buffer 前做整片 `dcache.ciall`（clean+invalidate），仍 stall（`dcache.iall` 不寫回髒行會直接讓韌體崩潰，改用 ciall） | 排除 |
| echo 訊息 header 的 dst 不匹配 ept 被丟 | 非侵入式全域探針（只寫記憶體、devmem 讀回）顯示 `found=1`、`dst=0x1e` 正確 | 排除 |
| host 端 vring 對映是快取造成不一致 | host 用 `ioremap()` 對映 vring（[cvi_rpmsg_shmem.c](../../linux/drivers/rpmsg/cvi_rpmsg_shmem.c)）= 非快取，寫入直達 DRAM | 排除 |
| 兩核對 `cpu_mbox_en` 非原子 RMW 互相覆寫 | host 與韌體都用同一把跨核硬體 spinlock（`SPIN_MBOX` via `hw_spin_lock`） | 排除 |

> 誠實記錄：這一輪排除法動用了乾淨斷電重測與 devmem／全域變數兩種**非侵入式**探法。中途踩到一個 Heisenbug——在 `virtqueue_get_available_buffer` 這條熱路徑加 printf，printf 在 124000 baud 每行約 3ms，卡住 env_isr 幾十毫秒、反而讓 `thread_rpmsg` 送不出去，改變了被觀測的行為。教訓：時序敏感路徑不能用 printf 插探，要用「只寫記憶體、事後 devmem 讀」的手法。

### 4b.3 定位到的漏點與修法（部分修復）

排除快取後，方向收斂到 host 端的 mailbox 通知→派發鏈：

- **漏點一：單槽 `rtos_cmd_id` + workqueue 合併。** [cvi_rpmsg_shmem.c](../../linux/drivers/rpmsg/cvi_rpmsg_shmem.c) 的 mailbox handler 把 vring 索引存進單一全域 `rtos_cmd_id`，再 `schedule_delayed_work`；work handler 只派送 `vq[rtos_cmd_id]`。由於 work 已排程時 `schedule_delayed_work` 是 no-op，兩個背靠背的通知會塌成一次 work、只服務最後一個 cmd_id，另一個 vq（vring0 的 echo）的 callback 被靜默丟掉。**修法：work handler 改輪詢所有 vq**（virtio callback 會自檢 used ring，多呼叫無害）。
- **漏點二：mailbox ISR 的 `set_val` 只快照一次。** [rtos_cmdqu.c](../../linux/drivers/soc/cvitek/rtos_cmdqu/rtos_cmdqu.c) 中斷處理開頭讀一次 `set_val` 就據以迴圈，處理期間到達的新訊息不在快照裡、只能指望 PLIC 重觸發。**修法：處理完重讀 `set_val`，用 do/while 排空。**

這兩個修法都正確，把往返從 **1 拉到數輪（2～4）**，但**沒有完全消除 stall**。

### 4b.4 通知層修法治標不治本 + 兩個失敗嘗試

兩個通知層修法（4b.3）正確但只把往返從 1 拉到數輪。殘留現象：乾淨首跑仍在 1～2 輪 stall，dmesg 洗版 `virtio_rpmsg_bus: uhm, incoming signal, but no used buffer ?`，devmem 顯示「韌體送 6、host 交付 3」。當時的兩個嫌疑——mailbox enable-bit 遮蔽 race、殘留 stale endpoint 搶走 echo——後來都被硬證據**證偽**：

- **enable-bit 遮蔽 race**：stall 當下 devmem 直讀 mailbox MMIO（0x01900000），en／int／**raw** 全為 0、8 個 slot 全空——硬體層沒有任何「latch 但被遮蔽」的 pending 通知。
- **stale endpoint 搶走 echo**：dmesg `msg received with no recipient` 計數為 0——沒有訊息在 ept 派發層被丟。

**兩次失敗的嘗試（誠實記錄）**：「自癒式後備輪詢」——定期輪詢 vq 把漏掉的 buffer 撿回。第一版用另一個 work item 呼叫 vq callback，與 mailbox work 的 `recv_done` 並發 race `last_used_idx`（virtio 的 vq 非可重入），反而更糟；第二版改用同一個 work item 自我重排避開並發，仍無效。兩版都撤除。**無效這件事本身是關鍵線索**：如果 echo 真的躺在 used ring 裡，每 2ms 的輪詢不可能撿不到——所以爛掉的不是「通知」，是**資料本身**。

### 4b.5 插探反噬：一次對自己工具的翻案

深挖期間曾在韌體加「非侵入」全域變數記錄（寫記憶體 + flush，devmem 讀回），得到「readcb 有跑但韌體不回 echo」的觀測，一度把矛頭指向韌體排程。後來發現**該插探韌體本身有毒**：那顆韌體從開機起 demo 執行緒就從未輸出、處理一則訊息後整核靜默——行為被插探改變了。回頭盤點：**所有「韌體收了不回」的證據都來自有毒的插探版；乾淨韌體的每一次實驗都有回 echo。**

> 教訓：printf 會擾動時序（§4b.2 已知），但「只寫記憶體 + flush」的插探也不保證無害。插探版韌體必須先驗證基線行為（demo 執行緒還在跑、多輪 mailbox 命令正常）再採信其觀測；更好的做法是讓觀測完全在對側（Linux devmem 讀共享記憶體與 MMIO），韌體保持原樣。

## 4c. Bug #2 真根因：一行 devicetree——`dma-noncoherent` 沒被繼承（已修復）

### 4c.1 收斂：爛的是資料，不是通知

把 4b 的所有硬證據放在一起看：

- vring 索引與 kick 都通（mailbox regs 乾淨、used/avail 有前進、env_isr 有觸發）；
- 自癒輪詢撿不回任何東西 → used ring 裡沒有「漏掉的好資料」；
- 韌體在乾淨狀態讀 msg2 的 avail->idx 得 1、host 說已寫到 2，而且**整片 dcache clean+invalidate（dcache.ciall）都救不了**。

最後一點是決定性的：如果 stale 資料在 C906L 自己的 cache，ciall 之後必然讀到 DRAM 新值。讀不到，代表 **DRAM 本身就沒有 host 的寫入**——問題根本不在讀端（C906L），在寫端（C906B）：host 的寫沒有落到 DRAM，卡在它自己的 D-cache 裡。之前 devmem 看到「host 已寫到 2」是**同核假象**——devmem 跑在 C906B 上，讀的是自己 cache 裡的髒資料。

### 4c.2 根因

檢查記憶體屬性鏈，找到斷點：

```dts
/* sg2002.dtsi（mainline 原有）*/
soc {
    dma-noncoherent;          /* SG2002 是非同調 SoC，屬性掛在 soc 節點 */
    ...
};

/* 我們的 board dts（移植加入的 AMP 節點）*/
/ {
    rpmsg { compatible = "cvitek,rpmsg"; ... };   /* 在根層級！*/
};
```

devicetree 的 `dma-noncoherent` 靠**節點祖先鏈**繼承。rpmsg 節點放在根層級（與 reserved-memory 平級、不在 /soc 底下），繼承不到——Linux 於是把它當 **DMA 同調**裝置，`dma_alloc_coherent()` 直接回傳普通可快取記憶體、之後零快取維護。

受害範圍剛好避開了所有先前的檢查：

| 記憶體 | 映射方式 | 是否受害 |
|---|---|---|
| vring metadata（desc/avail/used，0x8fc00000）| `ioremap()`（no-map 保留區）| 否——真 uncached，所以索引、kick 全程正常 |
| **256KB 訊息緩衝池**（`dma_alloc_coherent`）| **cached 一般記憶體** | **是**——訊息 header+payload 落 DRAM 全看隨機 eviction |

這解釋了每一個歷史症狀：msg1 常好（開機早、cache line 早被逐出）；msg2 之後時好時壞（看 eviction 運氣）；跨 run 累積惡化（越多髒 line 越亂）；六版通知修法只能改善不能根治（通知層本來就只是次要漏點）；dcache.ciall 無效（髒的是**對面**核的 cache）。vendor 5.10 沒事，因為 vendor kernel 全域走 T-Head 非同調路徑。

### 4c.3 修法與驗證

修法一行（外加 probe 一行診斷列印）：

```dts
rpmsg {
    compatible = "cvitek,rpmsg";
    ...
    dma-noncoherent;    /* 根層級節點繼承不到 soc 的，就地補上 */
};
```

生效條件驗證：uncached 映射在 C906 上依賴 **T-Head MAE errata**（`CONFIG_ERRATA_THEAD_MAE=y`，runtime 三道門檻：marchid==0、mimpid==0、`th.sxstatus` 的 MAEE bit）。開機列印確認：

```
cvi_rpmsg: th.sxstatus=40638000 (MAEE=1) dev_is_dma_coherent=0
```

實測（乾淨斷電開機）：

| 測試 | 結果 |
|---|---|
| mailbox_pingpong 首跑 | **16/16，exit 0** |
| 連續 5 次重跑 | 全部 16/16 |
| remoteproc stop/start 熱重啟後 | 16/16 |

跨 run 累積惡化的現象也一併消失——證實那也是 cached buffer 的 eviction 垃圾，不是另一個 bug。

修法已 commit（branch sg2002-rpmsg-port）：

- `0ad3121fa4cd` riscv: dts: sg2002: mark Duo256M rpmsg node dma-noncoherent（根因）
- `67f6e5f7d09d` rpmsg: cvi: service all virtqueues from the notification work handler（次要漏點，保留為強健化）
- `8f9b9fe9a8c6` soc: cvitek: rtos_cmdqu: drain mailbox status in the interrupt handler（同上）

> 移植教訓（本 bug 的可泛化形式）：**把 vendor 驅動的節點加進 mainline DTS 時，節點放的位置決定它繼承到哪些匯流排屬性。** vendor kernel 常在 arch 層全域處理同調性，driver 移植過去「能動」，但屬性鏈斷了就會以最難除錯的形式（隨機時序、跨核不一致）發作。凡是共享記憶體的 AMP 裝置節點，`dma-noncoherent`／`dma-coherent` 要當第一檢查項。

---

## 4d. Bug #3：開機樂透式 mailbox 全聾——edge-latch 陷阱（已修復）

Bug #2 修復後某次開機，pingpong 又掛了——但簽名完全不同：**第 0 輪就死**（v1 avail=1、used=0，小核連第一則都沒消費），小核本身活著（demo thread 正常跳動），且 stop/start 救不回、訊息越積越多（int2 從 0x1F 漲到 0xFF、8 個 slot 全滿到 Linux 端 `ioctl error!`）。

### 4d.1 定位

devmem 直讀 mailbox MMIO：`en2=int2=raw2=0x1F`——五個中斷 **pending + enabled + 已 latch**，從這次開機的第 6.6 秒（NS announce 後的第一批 kick）起就沒人處理。也就是說這輪開機小核的 mailbox IRQ（61）從頭到尾聾的。

先試了「PLIC stuck claim」理論（韌體 init 盲寫 complete 全部 ID）——**證偽**，重載後依然聾，已撤除。

決定性實驗（免重編）：從 Linux 端把 mailbox 清乾淨（`devmem 0x01900030 32 0xFF` 清 int latch + 清零 8 個 slot），讓中斷線放下來，再送一發新 kick——**小核 ISR 當場醒來**（serial 立刻出現 `recv cmd(19)`）。

### 4d.2 根因與修法

**mailbox→C906L 的中斷是 edge-latch 行為**：若韌體初始化時中斷線已經是高的（有 kick 塞在 mailbox 裡——例如 Linux 在 remoteproc stop 與 start 之間送了 kick，正是 S98 重啟流程的窗口），韌體 IRQ init 清掉 latch 的 pending 後線仍持續高——**再也不會有新 edge**，這輪開機 mailbox RX 永聾。「開機樂透」= host 的 kick 有沒有搶在韌體 init 之前到。

修法（韌體 comm_main.c，`request_irq(61)` 之前）：先 quiesce mailbox——清 int latch、清 enable bits、清零 context slots——保證線是低的，第一個真 kick 就有新 edge。

驗證（決定性、非賭運氣）：**故意重現觸發情境**——停韌體 → 停著時送 kick（int2=0x1，線掛高）→ 啟動新韌體 → int2 被 init 自清為 0 → pingpong 16/16。舊韌體在同一情境下必聾。修法 commit：threadx-amp `b1d7c64`。

> 教訓：remoteproc stop 只 reset 核心，**不 reset SoC 層的共享資源**（mailbox latch、PLIC、DRAM）。韌體必須把「前世殘留」視為初始化的一部分。

---

## 5. 重現與驗證步驟

### 5.1 重現原 bug（修法前的 kernel）

前置：序列 host 端設 baud 124000；udev 規則放行 /dev/ttyUSB0。

1. 實體斷電重開取得乾淨 channel（`adb reboot` 是 no-op，見 §2.2）。
2. host 端唯讀擷取序列（124000）。
3. adb 執行 `mailbox_pingpong`（dst 0x1e）。
4. 觀察：pingpong 印出 1～4 筆 `ThreadX_data.DATA` 後 read 永久卡死；輪數隨機、跨 run 惡化。
5. stall 當下 `devmem` 讀 vring 索引與 mailbox MMIO：索引有前進、mailbox 乾淨——資料層爛、通知層好，即 §4c 的簽名。

對照組：`mailbox_test`（CMD_DUO_LED，資料走 mailbox MMIO 暫存器而非 DRAM buffer）永遠成功——**天生免疫**，因為 MMIO 不經 cache。這個對照組其實從第一天就在指向快取同調性，只是當時沒讀懂。

### 5.2 驗證修法

1. kernel 含 commit `0ad3121fa4cd`（DTS `dma-noncoherent`），重刷 boot.sd 後實體斷電。
2. `dmesg | grep cvi_rpmsg` 確認 `MAEE=1 dev_is_dma_coherent=0`。
3. `mailbox_pingpong` 連跑多次 + `echo stop; echo start` 熱重啟後再跑：全部 16/16。

---

## 6. 現況與待辦

已完成並驗證：
- Task 1（開機時序根因）、Task 2a（S98 開機自動重啟）、Task 3（rtos_cmdqu LED 路徑）、Task 4a/4b（USB peripheral + adbd）。
- 序列可視化（baud 124000）與韌體插探流程建立。
- **Task 2b Bug #1（ioctl enum 錯位）根因定位並修復驗證**：mailbox_test.c 列舉對齊核心後不再覆蓋 RPMsg handler。
- **Task 2b Bug #2 根因定位並修復驗證（§4c）**：rpmsg DT 節點缺 `dma-noncoherent` → 訊息緩衝區被當同調 cached 記憶體。一行 DTS 修掉，pingpong 16/16 穩定（連跑 6 次 + 熱重啟全綠）。三個 kernel commit 落版控。
- **雙核 RPMsg 通訊（本移植的核心目標）在 mainline Linux 7.0-rc6 上完整打通。**

- **Bug #3（edge-latch 開機樂透）根因定位並修復驗證（§4d）**：故意重現觸發情境驗證，commit b1d7c64。
- **Task 5 延遲量測與第一輪優化完成（§7）**：decomp 拆解體系建立，155→~133µs（-14%），A1/B2 落地 commit。
- **Payload sweep 完成（§7.4）**：韌體支援 496B 可變長 echo（ff51694），4→496B 曲線 152→162µs（+6.6%），成長全在 host wake（+6.4µs）與韌體 memcpy（+2.0µs），transport 持平；斜率比 Henry 平（+10.1 vs +17.9µs），常數項仍輸 ~30µs。
- **Upstream remoteproc 打通 + SEC_SYS clock 根因修復（§8）**：Junhui Liu v2 系列上板
  （+3 個 review 修正），adb 暴斃事件三層翻案到底——真根因是 `clk_efuse` 被
  `clk_disable_unused` 關掉、SEC_SYS 任何存取懸死整板（一行 devmem 重現）；
  正式修法 sec_sys syscon 掛 `clocks = <&clk CLK_EFUSE>`（commit 36da34aaed2a），
  start/stop/start + pingpong 全過，`clk_ignore_unused` 退役。

- **RPMsg 全 upstream 化完成（§9）**：remoteproc vdev + 標準 virtio_rpmsg_bus +
  mainline cv1800-mailbox，vendor glue 退役；五層根因鏈（NS-spin、masked-window、
  pre-scheduler IRQ、儀器自殺、rproc_virtio dma_coherent）；300/300 pingpong，
  RTT ~170µs。upstream 素材累計三發。

待辦：
- lore 素材三發整理成 patch 系列（clk_efuse、mailbox EN 覆寫、rproc_virtio dma_coherent）+ v2 review 修正。
- RTT 差距分析（native 170µs vs glue 155µs：mbox framework txdone_poll 嫌疑）。
- 優化續攻：wake 細拆、384→416B 台階（需在 native 路徑重量測）。
- 筆記整理發表（HackMD）。

補充方法論筆記（供後續除錯）：
- **只有實體斷電才真正重置**小核與 channel；`adb reboot` 是 no-op（無 restart handler）。從 running 狀態下 `echo stop; echo start` 可乾淨重啟小核並重載韌體，供韌體迭代（但不清 DRAM vring 殘留，索引仍會累積）。
- **`unbind`/`bind` cvi-rpmsg 驅動**可重跑 probe、重新登記 RPMsg 的 `rtos_cmdqu_handler`，用來解除 Bug #1 式的 handler 覆蓋、或驗證是誰覆蓋了它。
- **序列讀取穩定、寫入不穩**（PL2303 clone 的 TX/RX baud 不對稱）：驅動板子用 adb，觀察小核用序列。
- **時序敏感路徑禁用 printf 插探**（會變 Heisenbug）：改用「只寫全域變數、事後從 Linux `devmem` 讀韌體符號位址（nm 取 offset）」的非侵入手法；vring 狀態直接 `devmem` 讀生產者／消費者索引（vring1 avail@+0x1000、used@+0x2000）。
- **kernel 迭代免讀卡機**：`/boot` 是 vfat rw（/dev/mmcblk0p1），重編 Image → lzma → `mkimage -f boot.its boot.sd` → `adb push /boot/boot.sd`，實體斷電生效（`adb reboot` 無效）。firmware 則 `adb push /lib/firmware/cvirtos.elf` + stop/start 熱換。

---

## 7. 延遲量測與優化（Task 5，2026-07-07）

方法採 HenryChaing 同款（HackMD: ei4rzk-OQfOreOa5fG2eKQ），對照他在 vendor 5.10 上的數據。原則：**先量測、再優化、再回量**，每一步有數字背書。

### 7.1 量測體系

- **端到端**：`rpmsg_rtt_bench.c`——userspace CLOCK_MONOTONIC，write→read 往返，clock overhead 校正，CSV schema 與既有 5.10 smoke 數據相容。
- **分段拆解（decomp4 級）**：`rpmsg_rtt_decomp.c`——三層插樁：
  - kernel：b1/b1_done（notify 前後）、b2（echo IRQ 進入）、b2b（派發 handler 開始），經 `/proc/rpmsg_rtt` 匯出；
  - 韌體：s1（mailbox ISR 進入）、s3（echo 送出前），rdtime 打戳，**24B echo 帶回**（舊 4B pingpong 相容，char-dev read 自動截斷）；
  - 跨核時基：兩顆 C906 的 time CSR 同為 25MHz，**正向+反向 transport 相加消掉 inter-CLINT offset**；每筆帶 residual 自洽檢查。
- 前置必修：拔掉韌體熱路徑 printf（每行 ~3ms @124000——Henry 也踩過同一坑，2ms→75µs）。
- 觀測開銷：探針全開比素身 bench 高 ~17µs（24B uncached echo + 打戳），decomp 數據只跟 decomp 數據比。

### 7.2 Baseline 與分解

| 配置 | RTT p50（10k 筆，4B）|
|---|---|
| Henry（vendor 5.10）| 75 µs |
| 本板 5.10 對照（同 u-boot 同時脈）| ~105 µs |
| **mainline 7.0 起點** | **155 µs** |

分解（kernel #9）：syscall_in 20.3 / notify 3.4 / transport 31.6 / **turnaround 13.2（與 vendor 持平——韌體端無罪）** / **workwait 38.9** / **wake 68.4**。host 收訊路徑（workwait+wake）佔 62%，就是差距所在。

### 7.3 優化迭代（含失敗記錄）

**A1 砍派發延遲——三次迭代：**
1. `system_bh_wq`（softirq）——**失敗**：`cvi_rpmsg_notify` 的 mutex 在 recv_done 回收 re-kick 時於 softirq 睡眠 → `BUG: scheduling while atomic`，channel 全斷。撤除。（也不能在 mailbox ISR 直呼：re-kick 會重取 ISR 正持有的跨核硬體鎖 = 死鎖。）
2. `system_highpri_wq`——**零效果**：workwait 38.8µs 紋風不動，問題不在佇列優先權。
3. **專屬 SCHED_FIFO kthread_worker——有效**：workwait 38.8→28.3、RTT p50 173.4→154.1（-11%）、p90 180→157.5。commit `08b4d72a`。

**B2 buffer 改 cached + 明確 streaming sync**（virtio_rpmsg_bus：池初始 clean、TX 寫後 clean、RX 讀前 invalidate）：RTT 154.1→150.5。4B payload 下增益小（copy 本來就小），結構性正確、大 payload 才是主場。sanity 16/16 證明 sync 點沒漏。commit `ee847cec`。

**cpuidle 假設——證偽**：此 kernel 無 cpuidle driver（idle=純 WFI），sysfs 檢查零成本排除。

### 7.4 Payload sweep：RTT vs 訊息大小（2026-07-07）

前置：韌體原本 `read_cb` 只收 ≤24B、echo 固定 24B，payload 只能量一個點。改為收滿 rpmsg buffer 上限 496B（512B vring buffer − 16B header），echo 同長度回去、**前 24B 照蓋 stamp header**——所以任何 payload 大小都能同時做分段歸因；4B 舊行為不變（sanity 152.6µs，與 B2 基線一致）。threadx-amp commit `ff51694`。bench：`rpmsg_rtt_payload.c`（4B + 32..496B，每檔 2000 筆），duo-examples commit `57affa1`。

結果（p50，µs）：

| payload | RTT | turnaround | wake | 其餘分段 |
|---|---|---|---|---|
| 4B | 152.0 | 13.6 | 63.2 | 持平 |
| 128B | 152.8 | 13.8 | 64.2 | 持平 |
| 256B | 153.3 | 15.1 | 64.4 | 持平 |
| 384B | 155.0 | 14.9 | 64.7 | 持平 |
| 496B | 162.1 | 15.6 | 69.6 | 持平 |

歸因（4B→496B，Δ +10.1µs）：
- **wake +6.4**：host 收訊交付路徑（vq callback → eptdev skb copy → copy_to_user）。其中 **384→416B 有一階 +4.7µs 的跳變**（非線性，疑似 skb/配置尺寸級距，待驗證）；416B 前只 +1.5µs——B2 cached buffer 讓 host 端 copy 幾乎免費。
- **turnaround +2.0**：韌體端兩次 memcpy（rx 進 buffer、tx 進 vring），~4ns/B，符合 C906L 走 uncached 共享記憶體的預期。
- transport / notify / workwait / syscall_in：**全部持平**——傳輸本體（mailbox kick + vring 指標操作）與 payload 大小無關，設計上就該如此，數據證實。

對照 Henry（vendor 5.10，decomp5）：32B 114.0 → 496B 131.8（**+17.9µs**），成長也集中在 Linux 收訊側（他歸因 +12µs 在 RX callback）。兩邊曲線同形——**常數項我們還輸 ~30-38µs（host RX 路徑），但斜率我們反而平（+10.1 vs +17.9）**，B2 的 cached buffer 在大 payload 這裡兌現了價值；gap 從 4B 的 38µs 收斂到 496B 的 30µs。

### 7.5 現況與下一步

**累計：155 → ~133µs（-14%）**。剩餘目標（decomp p50）：wake 62.7（最大塊，需 decomp5 級細拆 recv_done/派發/喚醒；payload sweep 顯示它幾乎全是常數開銷，不是 copy）、workwait 28.5（rtos_cmdqu ISR 尾段在硬體鎖下的 slot 排空）、transport 25.4（vs Henry 17.2）、384→416B 的 wake 階梯跳變成因。

數據存檔：`report/current-7.0-fixed/`（baseline、rtkthread、b2cached 三份 10k CSV + summary、`rtt_payload_sweep.csv` 17 檔×2000 筆）。

---

## 8. Upstream remoteproc 之路：adb 暴斃事件與 SEC_SYS 時脈根因（2026-07-13〜14）

前七章的系統跑在 vendor 移植版的 rproc/rpmsg glue 上。本章記錄把 C906L 的生命週期管理換成
**upstream 候選 driver**（Junhui Liu 的 remoteproc v2 系列，2025-07-28 起停滯在 patchwork
state "new"）的過程——以及它引爆的、整個專題最深的一個坑：一個 driver 完全無辜、
根因在 clock framework 層的整板懸死。誠實記錄三層翻案與兩個假陽性。

### 8.1 素材與準備

- v2 系列兩個 patch（binding + driver，共 239 行）以 `git am` 套用、保留原作者署名。
  小插曲：patchwork 的 `/mbox/` 端點被 Anubis 擋，改從 REST API 的 JSON 重組 mbox。
- 套用時順手修了三個 review 缺陷：sparse `__iomem` cast、binding 的
  `additionalItems: true`（Krzysztof 已點名）、以及 `devm_rproc_add` 配上 `.remove`
  裡手動 `rproc_del` 的 double-delete。
- DTS 接線：新增 `sec_sys` syscon（0x020B0000，C906L enable bit13 + bootaddr @0x20，
  即 vendor FSBL 開小核所碰的暫存器）+ rproc 節點（`resets = <&rst RST_CPUSYS2>`）。

### 8.2 adb 暴斃與三層翻案

新 kernel 一開機，adb 死透。序列埠只能讀不能寫（PL2303 TX 故障），一時之間整塊板
像磚。第一反應全錯：懷疑板子燒了、懷疑 USB 線是充電線（**冤枉了線材**——使用者指出
掛掉前用的就是同一條，回頭檢討才走上正軌）。之後靠 SD 卡鑑識（讀卡機改 init script、
在 rcS 埋每支腳本的 BEGIN/END trace）一層層剝：

1. **第一層：rcS 開機鏈凍結。** 真正啟動 adb 的不是 S97adb（那只做 gadget probe），
   而是 S99user → `/mnt/system/usb.sh` → `usb-adb.sh`（mount functionfs、起 adbd、
   寫 UDC）。S98cvirtos 對 rproc 的 sysfs 操作把 rcS 卡死 → S99user 永遠不執行 →
   adb 根本沒被啟動。改成 S97 先起診斷 adb、S98 丟背景 subshell 後，adb 活了，但——
2. **第二層：「`echo start` D-state 懸死」是誤讀。** trace 斷在 start 之後，一開始
   解讀為 sysfs 寫入卡死。後來發現連 `timeout` 包住的背景 trace 追加、`insmod`、
   functionfs 設定**全部**凍住——不是那一個 write 卡住，是 start 之後所有碰 rootfs
   的 I/O 都死了。症狀在檔案系統層，兇手不在。
3. **第三層（真根因）：driver 無辜。** 決定性實驗：在**完全不含 upstream driver**
   的已知良好 kernel 上，一行 `devmem 0x020B0004`（純讀）就讓整板懸死——MMC 陪葬、
   rootfs I/O 全滅。任何人碰 SEC_SYS 都會死；upstream driver 只是開機後第一個碰的人。

兩個假陽性也記錄在案：

- **「ThreadX 在跑 = driver 有效」——假。** FSBL 上電就把 C906L 開起來了
  （`fsbl/plat/cv181x/platform.c`），driver 沒做事韌體照樣跑。真驗證要看
  stop/start 之後韌體序列時間戳**歸零**。
- **`olddefconfig` 在切分支後悄悄洗掉 `CONFIG_SOPHGO_CV1800B_C906L`**，第一顆驗證
  kernel 根本沒有 driver，而 uname 版號同為 00016 無法區分——靠
  `grep -c c906l /proc/kallsyms`（0 vs 8）才抓到。教訓：每次 olddefconfig 後驗 kallsyms。

### 8.3 兇手：`clk_disable_unused` 對上沒寫進 DT 的時脈依賴

機制拆解：

- mainline 在開機尾聲（late_initcall）跑 `clk_disable_unused()`：**沒有任何 DT 節點
  認領的 clock 一律關掉**。這是刻意設計（省電 + 逼 driver 誠實申報），vendor 5.10 沒有
  這個行為。
- SEC_SYS 暫存器區塊（0x020B0000）的匯流排介面吃 **`clk_efuse`（CLK_EN_0 bit 11）**。
  這顆 SoC 對「存取無時脈區塊」的反應不是 bus error，而是 AXI interconnect 直接懸死。
- 證據鏈在 vendor 自己的 code 裡：`linux_5.10/drivers/crypto/cvitek-spacc.c` 每次碰
  0x020b0000 前後手動 `clk_prepare_enable(clk_efuse)` / disable——vendor 知道這條依賴，
  但用 `clk_get_sys(NULL, "clk_efuse")` 繞過 DT 硬編在 C 裡，device tree 隻字未提。
  在 vendor 封閉花園裡沒人驗收這份合約；一到 mainline 的規則下就是地雷。
- 四個條件同時成立才會踩到：跑 mainline（有 clk_disable_unused）×
  開機完成後才碰 SEC_SYS（sysfs start）× binding 沒申報 clock（v2 系列的缺）×
  沒掛 `clk_ignore_unused`。原作者大概在 probe 階段 auto-boot（clock 還沒被關）
  或帶著 `clk_ignore_unused` 測試，所以連他自己都沒踩到。

過渡驗證：`CONFIG_CMDLINE="clk_ignore_unused"` 烤進 kernel（u-boot env 不可達，
FIT 無 bootargs）→ 全系統在 upstream driver 上打通：S98 start/stop/start 全 rc=0、
韌體時間戳歸零（真・driver 重開小核）、pingpong RTT ~153µs。但這是大槌——
整個節能機制陪葬，upstream 不可能收。

### 8.4 正式修法：把依賴寫回 device tree 該在的位置

```dts
sec_sys: syscon@20b0000 {
        compatible = "sophgo,sg2002-sec-sys", "syscon";
        reg = <0x020b0000 0x1000>;
        clocks = <&clk CLK_EFUSE>;
};
```

一行 `clocks`。mainline syscon core（`drivers/mfd/syscon.c`）會把第一顆 clock attach
到 regmap-mmio，之後**每次 regmap 讀寫自動在前後 `clk_enable`/`clk_disable`**
（just-in-time）——rproc driver 一行 C 都不用改。commit `36da34aaed2a`，
`clk_ignore_unused` 從 cmdline 移除。

驗證（gate 硬體狀態 = N 之下）：

```
clk_efuse   0(en) 1(prep)  25000000  N   syscon@20b0000   ← consumer 掛上、idle 時 gate 關閉
start1 rc=0 state=running / stop rc=0 / start2 rc=0       ← 開機自動 S98 全過
pingpong RTT p50 ~155µs                                    ← 通訊正常
```

注意 `devmem` 直讀 SEC_SYS **仍會懸死**（bypass regmap）——行為正確，勿當回歸測試。

風險控制值得一記：測試 kernel 上板前先停用 S98 自動 start（開機路徑上沒有任何東西
碰 SEC_SYS）→ 就算 clock 猜錯，斷電重開即恢復，永遠不需要拆卡救援；卡上常備三顆
kernel（clkfix / ignoreunused 備援 / 會懸死的對照組，後者檔名直接改成 `*-WEDGES-*` 防誤用）。
另外這塊板的 warm reboot（`reboot` 指令）會卡在 FSBL 不回來，驗證一律實體斷電。

### 8.5 收穫

- **症狀層 ≠ 兇手層**：I/O 全死的根因在 clock framework，中間隔了「匯流排懸死 →
  MMC 死 → rootfs 凍結 → init 鏈斷 → adb 沒起」五層轉譯。每層都要用實驗釘死再往下。
- **最小重現是king**：把「upstream driver 掛了 adb」壓縮成「一行 devmem 懸死好 kernel」，
  嫌疑範圍瞬間從 239 行 driver 縮成 SoC 硬體行為。
- **翻案要誠實列帳**：線材冤案、ThreadX 假陽性、D-state 誤讀——記下來，下次省時間。
- 產出全部可回饋 upstream：v2 系列三個修正 + double-delete 發現 + clock 依賴 bug
  報告（含最小重現與 syscon clocks 修法）+ SG2002 支援 + 真機 Tested-by。

---

## 9. RPMsg 全 upstream 化：五層根因鏈的一夜（2026-07-14）

§8 之後系統是「一半 upstream」：rproc 用 upstream driver，RPMsg 仍騎 vendor glue
（`cvi_rpmsg_shmem.c` 自管 virtio 接線 + `rtos_cmdqu.c` 自管 mailbox）。本章記錄
把這一半也換掉的過程——目標架構：

```
virtio_rpmsg_bus（標準，不動）
    └── remoteproc vdev ← 韌體 resource table 自動生成
            └── rproc driver + .kick（本章新增）
                    └── cv1800-mailbox（mainline 已在樹內，首個 in-tree 使用者）
```

設計文件（gap 分析 + 六個決策）另存於專題 repo `docs/rpmsg-native-design.md`。
gap 分析的關鍵發現：mainline mailbox driver 與 vendor/韌體是**同一塊硬體的同一套
暫存器協定**（bit 對 bit 相同），CPU 編號 0=CA53/1=C906B/2=C906L；host kick 可以
偽裝成 vendor cmdqu 格式讓韌體 RX 零改動；韌體只需要 resource table（vring 位址
沿用 rpmsg-lite 硬編的 0x8fc00000/0x8fc08000）和固定 slot 的 TX kick。

kernel 端一次到位：driver 加 `.kick`/mbox client（~100 行，optional——無 mboxes
屬性就退回 standalone，對 upstream 是乾淨增量）、DTS 加 mailbox 節點、vring
保留區改名 `vdev0vring0/1`（remoteproc core 靠 carveout **名字**對位固定 da——
stm32/imx 慣例，名字錯了 core 會自己另配位址，韌體就對不上）、rproc 節點加
`dma-noncoherent`（§4c 的教訓，這次記得了）。第一次上板 vdev 就活了：

```
rproc-virtio rproc-virtio.0.auto: registered virtio0 (type 7)
virtio_rpmsg_bus virtio0: rpmsg host is online
```

——然後 pingpong 不通。接下來是一夜五層的剝洋蔥，每一層都有鐵證、每一層都不是
最後一層。

### 9.1 R1：韌體在 NS announce 時無界 spin（自己寫的坑）

我給韌體 `platform_notify` 寫的固定 slot TX 以「host 清 EN bit7」當 tx-done，
無界等待。但 mainline mailbox driver 的 ISR 用 `writeb(~valid, EN)` **整包覆寫**
enable 暫存器——收 slot N 時把其他七個 bit 全拉高。韌體開機送 NS announce →
spin 等一個永遠不會清的 bit → prio 6 執行緒把更低優先權全餓死。
修法：spin 加上限；kick 遺失由 host「收到任何 mailbox 中斷就 poke 兩個 vq」
補償（§4b「輪詢所有 vq」的老哲學，spurious 無害、lost kick 致命）。

### 9.2 R2：Bug #3 借屍還魂（masked-window edge 遺失）

host 的 mailbox framework 會把送不出去的 kick 排隊，靠 poll timer 等 tx-done
（= 韌體清 EN）再補送。韌體重開、quiesce 清 EN 的瞬間，host 立刻補送——正好落在
「quiesce 之後、PLIC unmask 之前」，edge 在沒人聽的時候過去了，韌體 mailbox
從此全聾（int 卡 0x03、ISR 進入次數 0）。§4d 的 edge-latch 陷阱一模一樣，
只是觸發者從「殘留 kick」變成「host 的排隊重送」。

### 9.3 R3：scheduler 起跑前的中斷（真正的擊殺者）

修完 R1/R2 韌體還是死，序列停在 `ptr:`（tx_application_define 結尾）——
**任何 thread 都沒起跑**。三個事實拼出機制：

1. mainline host 在 rproc start 後**毫秒級**就 kick（virtio probe 填 rx buffer
   後 `virtqueue_kick`）——vendor 時代從來沒有「韌體開機期間被 kick」這回事
2. C906L 的 **PLIC enable 跨 remoteproc stop/start 存活**（前一個韌體 instance
   `request_irq` 過，reset 不清 PLIC）
3. ThreadX port 在 `tx_kernel_enter` 內部才開 `mie.MEIE`——那一瞬間 pending
   中斷立刻觸發，而 **scheduler 還沒有任何 thread context**，port 的 ISR
   出入口組語直接把排程器毀掉

修法：main 第一行就 `disable_irq()` + quiesce（把線壓低、PLIC 關掉），
`request_irq` 搬進 mail thread——第一個中斷必然在合法 thread context 被接住。
決定性二分實驗：**把 resource table 的 section 名字改掉**（host 認不得 →
不建 vdev → 不 kick），韌體其他 byte 完全相同 → 開機完美。「host 互動」與
「韌體 image」一刀切開。

### 9.4 R4：偵錯工具自殺 + devmem 的 cache 謊言（方法論章）

R3 修完，「凍死」還在——但這次的兇手是**我自己的偵錯儀器**：

- 給 thread_1 加的 DBG printf 有 9 個參數，RISC-V varargs 溢出到 stack，
  在 1KB 的 thread stack 上直接疊爆、殃及整個 byte pool。序列停在
  「threadx 1 running: 10」之後那行——正是這個 printf。拆掉即癒。
- 更早的誤判全來自 **devmem 讀韌體全域變數是謊言**：C906L 對自己 image
  區的寫入躺在它的 D-cache（devmem 讀 DRAM 看到舊值），而 host 的 devmem
  對 RAM 走的也是 **C906B 的 cached 視角**（不是真 DRAM！）。兩層 cache
  夾擊，「counter 凍結」「probes 全零」全是幻覺，韌體其實活得好好的。
  可信探針的正確做法：放在共享窗口的空隙位址 + 韌體寫完顯式
  `clean_dcache_range()`。

### 9.5 R5：mainline bug——rproc_virtio 不繼承 dma_coherent（終局）

儀器可信之後，管線 X 光一次照穿：kick 全通（ISR→env_isr 計數吻合）、
RL 有消費訊息（vring used 前進）、NS announce 都到了 host
（`creating channel ... addr 0x1e`）——但 endpoint 分發計數 = 0。
在 rpmsg-lite 分發點記錄它讀到的 header：**dst=0, src=0, len=0**，全零；
而 host 側看訊息完好（dst=0x1E）。host 寫的資料沒到 DRAM。

根因在 mainline `remoteproc_virtio.c`：`rproc_virtio_probe()` 給自動生成的
vdev 平台裝置繼承了 `dma_range_map`、`dma_ops`、DMA mask——
**唯獨漏了 `dma_coherent` 旗標**。這個裝置沒有 OF node，拿到架構預設
（coherent）→ 我們 §7 的 B2 優化（cached buffers + 顯式 `dma_sync`）在
vdev 上全部靜默變 no-op → 訊息卡在 C906B 的 cache。vendor glue 時代沒事，
因為那時 vdev 的 parent 是 glue 自己的平台裝置（有 `dma-noncoherent`）。

一行修正（繼承旗標）→ **300/300 pingpong，RTT p50 ~170µs**。
這個 bug 影響所有「dma-noncoherent 平台 + remoteproc virtio」的組合，
是本專題目前最有份量的 upstream 發現。

### 9.6 結果與收穫

- **RPMsg 堆疊 100% upstream 架構**，vendor glue 兩支 driver 退役。
  RTT ~170µs vs glue 版 ~155µs（+15µs，mailbox framework 開銷，待分析）。
- kernel 4 commits（rpmsg-native branch）+ 韌體 2 commits；
  upstream 素材累計三發：clk_efuse 依賴（§8）、mailbox EN 覆寫 + probe
  hygiene、rproc_virtio dma_coherent。
- 方法論教訓，比結果更值錢：
  1. **雙核系統的「觀測」本身要先驗證**——devmem 在兩層 cache 之間讀到的
     可以全是幻覺；探針要選 uncached 位址 + 顯式 flush。
  2. **偵錯儀器是程式碼**——1KB stack 上的 9 參數 printf 殺掉的時間比它
     救回的多；每輪只加最小儀器，並先驗證儀器本身。
  3. **控制變因的二分要「改一個 bit 換一個世界」**——藏 section 名字讓
     host 行為 on/off，比任何 code 註解都乾淨。
  4. 「不動的舊端 + 新的另一端」不代表舊端不用改：host 的行為時序
     （開機即 kick、排隊重送）就是新協定，韌體的啟動路徑必須重新審視。

---

## 10. Native 路徑效能分析：mailbox poll hrtimer 風暴（2026-07-15）

§9 收尾時 native 路徑 RTT ~170µs，比 glue 優化版的 152µs 慢。本章把差距拆到
每一微秒，並揪出第四個 mainline 級問題。

### 10.1 基線與探針重建

先量基線（2000 iters/size）：**4B p50 = 176.4µs**（+24.4 vs glue），
p99 尾巴異常重（~300µs）；韌體 turnaround 與 glue 時代相同（13.1 vs 13.7µs）
→ 差距 100% 在 host 側。

decomp 探針隨 glue 退役，在新路徑重生（保持 `/proc/rpmsg_rtt` 同一格式，
userspace bench 零改動）：`b1/b1d` 夾住 rproc `.kick` 的 `mbox_send_message`、
`b2/b2t` 打在 mailbox 硬中斷、`b2b` 打在 rx callback 分發前——
`wwait = b2b - b2` 直接量出 threaded-IRQ 喚醒成本。

### 10.2 第一輪 decomp：自相矛盾的數字即是答案

| 段位（4B p50） | 數值 |
|---|---|
| RTT | 183.7µs |
| notify（`mbox_send_message`） | **140.7µs** (!) |
| hostrx（硬中斷→userspace） | **125.5µs** (!) |
| 各段總和 | 324µs ≫ RTT |

段位總和超過 RTT = 各段**時間重疊**，代表有東西在偷牆鐘。順藤摸瓜讀
`drivers/mailbox/mailbox.c`：

- cv1800-mailbox 用 `txdone_poll`，但**沒設 `txpoll_period`**（=0）；
- framework 的 poll hrtimer 在 tx 未完成時
  `hrtimer_forward_now(hrtimer, ms_to_ktime(0))` → **零週期連環觸發**，
  hardirq context 的 busy-loop 一路打到韌體清 EN bit 為止；
- C906B 是單核——風暴期間整顆 CPU 被 hrtimer 偷走，kick 路徑、echo
  接收路徑、userspace 全部在慢動作中執行。140µs 的
  `mbox_send_message` 和 300µs 的 p99 都是受害者，不是兇手。

pingpong 每回合兩次 kick（TX + rx-buffer 補貨）讓風暴窗口加倍，
第二發還會因第一發未 tx-done 被排進佇列等 hrtimer 撈。

### 10.3 修法：TXDONE_BY_ACK fire-and-forget

kick 本來就冪等（host 端「收到任一 mailbox 中斷就 poke 全部 vq」）：

- **host**：`client.knows_txdone = true`（framework 轉 TXDONE_BY_ACK），
  `mbox_send_message` 後立刻 `mbox_client_txdone()`——hrtimer 完全不啟動、
  佇列零等待（commit 32257ff8eecd）；
- **韌體**：對稱地「收到任一 host kick 就 `env_isr(0)+env_isr(1)`」，
  讓共用 slot 的 kick 合併變得無害（commit 2a652bd）。

### 10.4 修後逐段對照（4B p50，2000 iters）

| 段位 | glue 優化版 | native 修前 | native ACK 修後 |
|---|---|---|---|
| **RTT** | **152.0** | 176.4 | **164.0** |
| sysin | 21.5 | — | 21.6 |
| notify | 3.1 | 140.7* | **5.1** |
| transport | 25.6 | — | 28.9 |
| turnaround | 13.6 | 13.1 | 14.2 |
| wwait | 28.2 | — | 27.8 |
| wake | 63.2 | — | 71.8 |
| p99 | — | 301 | 220 |

（* 修前段位互相重疊，僅供病徵參考）

剩餘 +12µs 的歸屬：wake +8.6（remoteproc vdev 的分發間接層
`rproc_vq_interrupt`/idr 查找 + poke-both 多掃一圈 used ring）、
transport +3.3（韌體固定 slot 的 EN spin 檢查）、notify +2.0
（mailbox framework 的佇列/鎖層疊 vs glue 裸寫暫存器）。
wwait 持平（27.8 vs 28.2）——mailbox 的 threaded IRQ（SCHED_FIFO/50）
喚醒成本與 A1 的 RT kthread 相當。

### 10.5 wake 細拆與 hard-IRQ 直送：反超手工 glue

wake 段（68.9µs）再切四刀（w1=recv_done 進入、w2=skb 入列+喚醒、
w3=讀者醒來）：

| 子段 | 4B p50 | 內容 |
|---|---|---|
| vqdisp | 1.6µs | `rproc_vq_interrupt` 間接層——先前歸咎它是冤枉的 |
| deliver | 22.8µs | dma invalidate + ept 查找 + skb 配置/拷貝 |
| **sched** | **31.1µs** | irq thread → 使用者行程的排程喚醒 |
| ret | 13.4µs | copy_to_user + syscall 返回 |

關鍵洞察：接收路徑有**兩次** thread 交接（硬中斷→mailbox threaded
IRQ=wwait 27.8µs、再→使用者行程=sched 31.1µs），但 mailbox 訊息只有
8 bytes，下游全是 irq-safe 路徑（virtio 慣例本就在 hardirq 跑 vring
callback）——threaded handler 純屬浪費。把收訊直送搬進硬中斷
（commit c57796af5eb4）：

| 版本 | RTT 4B p50 | p99 |
|---|---|---|
| vendor 5.10（Henry） | 114.0 | — |
| glue 優化版 | 152.0 | — |
| native ACK 模式 | 164.0 | 220 |
| **native hard-IRQ 直送** | **127.6** | **178** |

wwait 27.8 → 1.7µs，**100% upstream 架構反超手工 glue 24.4µs**，
距 vendor 5.10 只剩 13.6µs；穩定性 1000/1000。

### 10.6 收尾三題：正確性自首、two-stage sync、p99 尾巴

**正確性自首**：hard-IRQ 直送讓 `rpmsg_recv_single` 的兩把 mutex
（endpoints_lock/cb_lock）、`GFP_KERNEL` 補貨、NS announce 的
`device_add()` 全部跑進原子上下文——能動是僥倖（mutex fastpath 無競爭、
單 sg 不配置）。修正（commit 6b86b6fa7475）：兩把鎖轉 irq-safe spinlock
（cb_lock 只有 virtio_rpmsg 使用，範圍可控）、GFP_ATOMIC、NS 頻道建立
延遲到 work item。效能零代價（131.3µs，在 127–131 的 boot-to-boot
變異內）、穩定性 1000/1000、NS 頻道正常。教訓：**把處理搬進更嚴格的
context 時，整條下游路徑的 sleeping 原語都要重新審計**——「能跑」不是
「正確」。

**two-stage rx sync**：收包 invalidate 從整顆 512B buffer 改為
「先 1 條 cache line（含 header+小 payload），長訊息才補刀」。理論省
~0.7µs，實測埋在雜訊裡——保留（原則正確），誠實記錄無可測差異。

**p99 尾巴破案**：p99−p50 ≈ 50µs 全在 wake/sysin 的隨機一段，突刺量級
一致（~55µs）。HZ=250 → tick 每 4ms 一次 → 131µs 視窗命中率 3.3% →
恰好從 p97 起飛，與實測（p90 +5µs、p99 +50µs）吻合。
**尾巴 = kernel timer tick，不是 bug**；單核無 NO_HZ_FULL 可逃。

### 10.7 結論

- hrtimer 風暴是真 bug（第四個 mainline 級發現候選：`txpoll_period = 0`
  的退化行為，影響所有未設週期的 polling mailbox controller——framework
  或 driver 至少一方該防呆）；修掉後 native 路徑淨賺 12.4µs。
- 「標準框架必然比手工 glue 慢」被推翻：架構稅其實集中在兩個
  可修的點（poll hrtimer 風暴、多餘的 threaded handler），修完後
  upstream 架構反而快 24µs。剩餘與 vendor 的 13.6µs 差距組成：
  sched 28.6（使用者行程喚醒的本質成本）、deliver 22.8、
  transport 26.9、sysin 20.4——每一微秒都有歸屬，沒有未解之謎。

---

## 附錄：本階段用到的工具（scratchpad）

- serial_cap.py / scapro.py：以 termios2 BOTHER 設任意 baud（124000）擷取序列，scapro 為唯讀。
- sweep.py：用 C906L 已知輸出當訊號，掃描找出正確 baud。
- led_sweep.c：唯寫地把 period 送到 LED endpoint 0x20（避開會 stall 的 echo 讀取）。
- 韌體插探：comm_main.c 內 prvQueueISR / prvCmdQuRunTask 的 `RT: DBG` 輸出；rpmsg_lite.c 的 `send: N ret=` 與 rx callback 進入點；build-cvirtos.sh 重建。
- **非侵入式全域探針**：在韌體熱路徑只寫全域變數（`g_dbg_*`），從 Linux `devmem <符號位址>`（`nm cvirtos.elf` 取位址）讀回，避開 printf Heisenbug。
- **devmem vring 檢視**：直接讀兩個 vring 的 avail/used 索引比對生產／消費（vring0@0x8fc00000、vring1@0x8fc08000；avail 在 +0x1000、used 在 +0x2000，idx 在該 32-bit 字的高 16 位）。
- host 端修法檔：drivers/rpmsg/cvi_rpmsg_shmem.c（work handler 輪詢所有 vq）、drivers/soc/cvitek/rtos_cmdqu/rtos_cmdqu.c（ISR set_val 排空）。
