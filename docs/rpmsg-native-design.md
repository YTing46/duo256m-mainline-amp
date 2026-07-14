# RPMsg 全 upstream 化設計（方向 2）

日期：2026-07-14。狀態：**已實作並驗證**（見文末後記；除錯全記錄在筆記 §9）。

## 目標

把現在「一半 upstream」的堆疊：

```
virtio_rpmsg_bus ── cvi_rpmsg_shmem.c（vendor virtio glue，自管 vring/kick）
                        └── rtos_cmdqu.c（vendor mailbox ISR + cmdqu 協定）
sophgo_cv1800b_c906l.c（upstream rproc，只管小核生命週期）
```

換成 100% upstream 架構：

```
virtio_rpmsg_bus ── remoteproc vdev（rsc table 驅動，core 自動建立）
                        └── sophgo_cv1800b_c906l.c + .kick（本設計新增）
                                └── cv1800-mailbox.c（mainline，已在樹內）
```

userspace 不動：`/dev/rpmsg_ctrl0` / `/dev/rpmsg0` 流程與 ioctl 完全相同
（vendor glue 本來就掛在 virtio_rpmsg_bus 上）。

## 事實依據（gap 分析結果）

1. **暫存器協定一致**：mainline `cv1800-mailbox.c` 與 vendor/韌體的
   `cvi_mailbox.h` 是同一塊硬體：`cpu_mbox_en[4]`@0x00、int_clr@0x10+cpu*0x10、
   int@+8、`mbox_set`@0x60、context 8 槽 × 8B @0x400。
   CPU 編號：0=CA53、**1=C906B（Linux）**、**2=C906L**、3=8051。
   binding `#mbox-cells = <2>`：`<channel target>`。
2. **rproc driver 已支援 rsc table**（`rproc_elf_load_rsc_table` +
   `find_loaded_rsc_table`，缺表時優雅退回 standalone）→ 只缺 `.kick`。
3. **韌體 kick 路徑**（ThreadX）：
   - RX（host→fw）：`prvQueueISR` 掃 8 槽、驗 `linux_valid==1`、丟 queue →
     task `switch(cmd_id)`：`0→env_isr(0)`、`1→env_isr(1)`。
   - TX（fw→host）：`platform_notify` → `mailbox_send((vector&1)+128)`，
     **動態選空槽**。
4. **cmdqu 8-byte 佈局**（little-endian）：
   byte0 `ip_id`、byte1 `cmd_id:7|block:1`、byte2 `linux_valid`、
   byte3 `rtos_valid`、byte4-7 `param_ptr`。
5. **vring 參數**（兩端已對齊，沿用）：da0=0x8fc00000、da1=0x8fc08000
   （`VRING_SIZE 0x8000`）、align 0x1000、num=`RL_BUFFER_COUNT`=256。
   buffer pool 由 host 配置，韌體從 descriptor 學位址 → pool 搬家無妨。

## 設計決策

### D1. host→fw kick：偽裝成 cmdqu，韌體 RX 零改動

`.kick(vqid)` 經 mbox channel `<vqid, 2>`（slot 0/1 → cpu2）送 8 bytes：

```c
u64 msg = ((u64)vqid << 8) | (1 << 16);   /* cmd_id=vqid, linux_valid=1 */
```

韌體的 `prvQueueISR` → queue → `env_isr(vqid)` 原路照走。

### D2. fw→host kick：固定 slot 7，淘汰動態選槽

vendor 的空槽掃描依賴「host 收完把 content 清零」——mainline driver **不清**
（ISR 只把本地指標設 NULL），8 發之後全槽看似占用、韌體 TX 餓死。
反正要改，就改乾淨：`platform_notify` 直接寫 slot 7
（content = cmdqu 格式 cmd_id=128/129，付 host 解析）、`en[1]|=BIT(7)`、
`mbox_set=BIT(7)`；TX-done = 等 host 清 EN bit。

**避開 mainline quirk**：`cv1800_mbox_irq` 收訊時 `writeb(~valid, EN_REG(1))`
整包覆寫——若韌體用兩個 TX slot，收 slot A 會誤把 slot B 的 EN 拉高，
韌體端 TX-done 判斷會永遠卡死。單一 TX slot 則只誤開沒人用的 bit（無 SET
不觸發，無害）。所以**韌體只用 slot 7**，vq 資訊放 payload。
（此 quirk 本身是 mainline bug 候選，記入 lore 素材。）

### D3. host RX callback：無條件打兩個 vq

fw 背靠背 kick 同槽可能 content 覆蓋（EN 已清但 threaded ISR 未讀）。
host 的 mbox rx callback 不信任 payload：一律
`rproc_vq_interrupt(rproc, 0)` + `rproc_vq_interrupt(rproc, 1)`。
virtio 會重查 used ring，spurious 無害——與 Bug #2 時代
「work handler 輪詢所有 vq」同一哲學，杜絕 lost-kick 死鎖。

### D4. resource table：韌體新增 `.resource_table` 段

vdev(VIRTIO_ID_RPMSG=7) + 2 vrings（da/align/num 用上面既有值，
**da 固定**，host 端 remoteproc 用 carveout 對位不重配）。
rpmsg-lite 的 `rpmsg_lite_remote_init(0x8fc00000, ...)` 組態不動。

### D5. DTS

- 新增 mailbox 節點（`sophgo,cv1800b-mailbox`、reg 0x01900000、
  IRQ 沿用 vendor rtos_cmdqu 的 85）——**cv1800-mailbox 首個 in-tree 使用者**。
- rproc 節點：`mboxes = <&mailbox 0 2>, <&mailbox 1 2>, <&mailbox 7 1>`、
  `mbox-names = "vq0", "vq1", "rx"`；memory-region 加入 vring 區
  （0x8fc00000 64KB no-map，補 label）；**加 `dma-noncoherent`**
  （節點在 DT root，不加就重演 Bug #2 的 cacheable buffer 災難）。
- 移除 vendor `rpmsg` / `rtos_cmdqu` 節點（與 mailbox 節點同 MMIO/IRQ，互斥）。

### D6. 相容性

driver 的 mbox 支援為 optional（無 `mboxes` 屬性 → 照 Junhui 原流程
standalone boot，不建 vdev 也不掛 kick）→ 對 upstream 是純增量，可獨立成 patch。

## 風險與回退

- kernel A/B：新 kernel 獨立 boot-*.sd，卡上保留 clkfix 版可隨時切回。
- 韌體 A/B：cvirtos.elf 保留舊版備份；rsc table 新增段不影響舊 kernel
  跑舊韌體（vendor glue 不看 rsc table）。
- LED demo / cmdqu ioctl 路徑會失效（rtos_cmdqu 移除）——接受，
  該路徑不屬於 rpmsg 目標堆疊。

---

## 後記（2026-07-14 深夜）：實作結果

設計如上實作並上板驗證：**300/300 pingpong，RTT p50 ~170µs**，全堆疊
（remoteproc vdev → virtio_rpmsg_bus → cv1800-mailbox）跑通，vendor glue 退役。

設計大致成立，但上板撞出**五層根因鏈**（完整記錄見筆記 §9）：

1. D2 的 tx-done spin 必須有界——host driver 的 EN 覆寫 quirk（D2 已預警）真的咬人
2. D3 的「host 排隊補送」與韌體 quiesce 時序互動，重演 Bug#3 的 edge 遺失
3. **設計時沒預料到的**：host 在 rproc start 後毫秒級就 kick + C906L PLIC enable
   跨 stop/start 存活 + ThreadX 在 scheduler 起跑前開中斷 = 韌體啟動路徑必須
   把 mailbox IRQ 的註冊延到 thread context（韌體端最大的追加修改）
4. 偵錯儀器本身連環翻車（devmem 的雙層 cache 謊言、9 參數 printf 疊爆 1KB stack）
5. **mainline bug**：`rproc_virtio_probe()` 不繼承 `dma_coherent` → vdev 上
   dma_sync 全 no-op → 小核讀到全零 header。一行修正，影響所有非同調平台 +
   remoteproc virtio 的組合——本專題最有份量的 upstream 發現

風險預案全部用上：三顆 kernel A/B、韌體雙版本、「藏 section 名」二分法。
