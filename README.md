# duo256m-mainline-amp

Milk-V Duo256M（SG2002，雙 C906 AMP）的異質多核通訊機制移植：
把 vendor 5.10 SDK 的 ThreadX + RPMsg + remoteproc 堆疊搬上 **mainline Linux 7.0-rc6**，
修掉一路上的通訊 bug 與一個 mainline 級的 clock 根因，並以量化數據對照 vendor 基線。

本專題復刻並延伸 [HenryChaing 的「Linux 核心專題: 異質多核通訊機制」](https://hackmd.io/@sysprog/linux2025-projects)。

## 成果總覽

| 項目 | 狀態 |
|---|---|
| mainline 7.0-rc6 在 Duo256M 開機（SD 卡、FIT image） | ✅ |
| USB gadget ADB（免序列線工作流） | ✅ |
| RPMsg host↔C906L pingpong 穩定通訊 | ✅（修掉 3 個 bug） |
| upstream C906L remoteproc driver（Junhui Liu v2）真機打通 | ✅（+3 個 review 修正） |
| SEC_SYS clock 根因（mainline 級 bug）+ 正式修法 | ✅ |
| RTT 分解量測 + payload sweep 對照 vendor | ✅ |

### 延遲數據（RTT p50, 10k iters）

| 平台 | 4B | 496B | 斜率 |
|---|---|---|---|
| vendor 5.10（Henry 基線） | 114.0µs | 131.8µs | +17.9µs |
| mainline 7.0（本專題，cached buffers + RT kthread） | 152.0µs | 162.1µs | **+10.1µs** |

常數項落後 ~30µs（wake 路徑，分解數據見 `bench/data/`），
大 payload 斜率反超 vendor（cached buffers + streaming DMA sync 的效果）。

## 修掉的關鍵 bug

1. **rpmsg 通知遺失（host 端，兩處）**：`rtos_cmdqu_handler` 用單一全域
   `rtos_cmd_id` + `schedule_delayed_work` 合併背靠背通知，只服務最後一個
   virtqueue → 改為輪詢全部 vq；mailbox ISR 的 `set_val` 只讀一次，漏接
   ISR 進行中抵達的訊息 → 改為 do/while 重讀排空。
2. **`dma-noncoherent` 缺失**：rpmsg 節點在 DT root、不在 `/soc` 之下，
   繼承不到 `dma-noncoherent` → virtio buffer 變 cacheable，C906B 的寫入
   滯留 D-cache，無 cache 的 C906L 讀到舊資料 → 時序性 1~數回合 stall。
3. **SEC_SYS clock 根因（本專題最重要的發現）**：SEC_SYS（0x020B0000，
   C906L 開機控制暫存器所在）的匯流排存取需要 `clk_efuse`（CLK_EN_0 bit 11）。
   mainline 的 `clk_disable_unused` 在開機尾聲把它關掉，之後**任何**對該區塊的
   load/store 都會懸死整條 AXI 匯流排（連 MMC 陪葬）——一行
   `devmem 0x020B0004` 即可重現。vendor 從未把這條依賴寫進 device tree
   （藏在 `cvitek-spacc.c` 的手動 `clk_prepare_enable` 裡）。
   修法：sec_sys syscon 節點加 `clocks = <&clk CLK_EFUSE>`，
   syscon 把 clock attach 到 regmap-mmio，每次暫存器存取 just-in-time 開關。
   詳見 `patches/linux/0013`。

upstream driver 本身另有三個修正（`__iomem` cast、binding `additionalItems`、
devm_rproc_add 與手動 rproc_del 的 double-delete），見 `patches/linux/0011`。

## Repo 結構

```
patches/linux/    13 個 patch：v7.0-rc6 → 完整可跑的 AMP 系統
                  （0009/0010 為 Junhui Liu 的 v2 系列，保留原作者署名）
patches/threadx/  C906L ThreadX 韌體修改（基於 HenryChaing/ThreadX-to-RISC-V64）
bench/            RTT 量測工具（總量/分解/payload sweep）+ CSV 數據
board/            板端開機鏈 scripts（S97adb/S98cvirtos/S99user、usb-adb.sh）、
                  boot.its（FIT）、驗證過的 kernel .config
docs/             工程筆記（含完整除錯歷程與翻案記錄）
```

## 重現步驟（摘要）

```sh
# kernel：v7.0-rc6 + patches，riscv64 toolchain
git checkout v7.0-rc6 && git am patches/linux/*.patch
cp board/linux7x-boot-verified.config .config   # 含 SOPHGO_CV1800B_C906L=y
make ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- Image dtbs
lzma -9 -k Image && mkimage -f board/boot.its boot.sd   # load addr 0x82000000
# boot.sd 放 SD 卡第一分割區；cvirtos.elf（ThreadX）放 rootfs /lib/firmware
```

序列埠：板端 console 115200（PL2303 注意 termios2 BOTHER）；C906L console 124000。
注意這塊板的 warm reboot（`reboot` 指令）會卡在 FSBL，要斷電重上。

## 參考

- [HenryChaing — Linux 核心專題: 異質多核通訊機制](https://hackmd.io/@sysprog/linux2025-projects)（vendor 5.10 基線）
- [Junhui Liu — remoteproc: Add C906L controller for Sophgo CV1800B SoC (v2)](https://lore.kernel.org/linux-remoteproc/)（upstream driver 素材）
- [milkv-duo/duo-buildroot-sdk-v2 v2.0.1](https://github.com/milkv-duo/duo-buildroot-sdk-v2/releases/tag/v2.0.1)（vendor 參考基線：FSBL、5.10 驅動對照）
- NCKU jserv Linux 核心設計課程
