# DISPLAY — the JH7110 display path and its acceleration (reference)

Research record for ROADMAP §10, gathered 2026-10-31 from primary sources (StarFive vendor Linux and U-Boot,
mainline Linux and U-Boot, the StarFive TRM memory map, a bare-metal bring-up on the same SoC). **Nothing here
is a measurement on this board yet**: every fact carries its source tag, and a fact becomes one of CLAUDE.md §3's
only when the board has confirmed it. Where sources conflict, both are recorded and the board decides.

Part 1 is the HDMI scanout bring-up; part 2 is what acceleration the chip offers.

## Measured on the board (2026-10-31)

What the board settled among the conflicting or unverified items below (`drivers/display_jh7110.c` logs each):

| Item | Answer on this board |
|---|---|
| HDMI rails (PMIC `0x11`) | `0xff` — ALDO3 1.8 V and ALDO5 0.9 V already on; nothing written to the PMIC |
| VOUT power-up (§2.1) | works as written: domains `0x03` → `0x13` |
| PMU / voutcrg in the firmware's tree | present (`starfive,jh7110-pmu`, `starfive,jh7110-voutcrg`); DC and HDMI are not |
| DC8200 after clocks/resets (§2.3) | revision `0x5720` |
| HDMI PLL tables (§3.2) | pre- and post-PLL lock at 1080p and 720p |
| Pixel clock (§2.4, open question 2) | the voutcrg divider of PLL2, mux 0, is enough: frames at 60 Hz |
| Commit convention (§4.4, open question 4) | StarFive's (`PANEL_CONFIG_EX` 1 while programming, 0 to apply) runs |
| `DP_CONFIG` (open question 3) | `0x0a` runs |
| `DISP_CURRENT_LOCATION` 0x1450 (§4.3, §6.7) | **reads 0 while running** — useless as evidence; vsync (`IRQ_EN`/`IRQ_ACK` bit 0) is |
| Full-frame Flush64 (§5) | ~2.0 ms for 8.3 MB |
| Hot-plug, EDID, picture | not measured — no monitor attached |

---

# Part 1 — DC8200 → Innosilicon HDMI TX

Date: 2026-10-31. Target: minikern on VF7110B1-2318 (VF2 v1.3B, 8 GiB), mainline U-Boot 2025.04.

## Legend

Every fact carries a tag:

- **[D]** documented: StarFive TRM, a DT binding, or a datasheet.
- **[C]** from driver code or a DT source. Real code that works, but not a spec.
- **[U]** uncertain: inferred, conflicting between sources, or not verified.

Source prefixes. All are pinned to the exact commit I read; the trees were cloned locally, so line numbers are exact.

| Tag | Tree | Permalink prefix |
|---|---|---|
| **SFL** | StarFive vendor Linux `JH7110_VisionFive2_6.6.y_devel` @ c09d915 (2025-08-18) | `https://github.com/starfive-tech/linux/blob/c09d91536baa616af1538a5bd2dbdf95c6e107d8/` |
| **SFU** | StarFive vendor U-Boot `JH7110_VisionFive2_devel` (v2021.10-based) @ c4c67bb (2025-09-15) | `https://github.com/starfive-tech/u-boot/blob/c4c67bb66ae6f41c98537d18cf5c3abc8b97b8e4/` |
| **ML** | Mainline Linux (v7.3 dev) @ 36aad21 (2026-10-30) | `https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/` |
| **MU** | Mainline U-Boot (2027.01 dev) @ 3401e00 | `https://github.com/u-boot/u-boot/blob/3401e006432abcbf7d73d7210f10df157f990ed3/` |
| **TRM** | StarFive JH7110 TRM, system memory map | https://doc-en.rvspace.org/JH7110/TRM/JH7110_TRM/system_memory_map.html |
| **LY** | Lup Yuen Lee, "RISC-V Star64 JH7110: Power Up the Display Controller with U-Boot Bootloader" (2023-09-02). Bare-metal pokes on the same SoC | https://lupyuen.github.io/articles/display3 |

---

## 0. Headline findings (read first)

1. **U-Boot 2025.04 on this board does NOT light HDMI.**
   - The board is running mainline U-Boot. `docs/hardware-first-boot.log` shows "U-Boot 2025.04" and `Out: serial@10000000` only, with no video device. [C]
   - Mainline U-Boot has no StarFive video driver: there is no `drivers/video/starfive/`, and `grep -ril "starfive|jh7110|dc8200"` in MU `drivers/video` returns nothing. [C]
   - Only the StarFive fork (SFU, 2021.10-based) has `drivers/video/starfive/sf_vop.c` and `sf_hdmi.c`.
   - Even the fork powers everything down before the OS starts. `sf_vop_remove` and `sf_hdmi_remove` are flagged `DM_FLAG_OS_PREPARE` and assert resets, gate clocks and switch the VOUT power domain off (SFU `drivers/video/starfive/sf_vop.c` L851–949, L984; `sf_hdmi.c` L543–560, L581). [C]
   - **Conclusion: minikern must do the whole bring-up itself.** Expect the VOUT domain off, its registers reading 0, at kernel entry. LY confirms reads of 0 before power-up. [C]
2. **The mainline DT passed by U-Boot has no `dc8200`, `hdmi`, `dssctrl` or HDMI pinctrl nodes.**
   - Mainline `jh7110.dtsi` has only `voutcrg`, `pwrc` and the `hdmitx0_pixelclk` fixed clock.
   - Minikern therefore has to hard-code those addresses. It cannot discover them from the tree.
3. **Mainline Linux gained `drivers/gpu/drm/verisilicon` (Icenowy Zheng) in v7.1.**
   - Evidence: `vs_dc.c` returns 404 at tag v7.0 and 200 at v7.1 on raw.githubusercontent.
   - It was merged to drm-misc-next on 2026-02-05 (lists.infradead.org linux-riscv 2026-February/085172).
   - **It only binds `thead,th1520-dc8200`.** There is no JH7110 DT node or binding. [C]
   - Its register headers are the cleanest naming of the DC8200 map, and I use them below.
4. **The JH7110 HDMI upstream attempts are not in mainline.**
   - Keith Zhao 2023: `starfive,jh7110-inno-hdmi`.
   - Michal Wilczynski RFC, Nov 2025: `starfive,jh7110-hdmi-mfd` plus `starfive,jh7110-inno-hdmi-phy` with `refoclk = xin24m`. Source: https://lkml.iu.edu/hypermail/linux/kernel/2511.1/10714.html
   - Neither is in ML: no files under `drivers/phy/starfive` or the bindings. [C]
   - The **vendor SFL 6.6 driver and the SFU U-Boot driver are the working references.** [C]
5. **There is a simpler pixel-clock path than the HDMI PHY.**
   - SFU drives the DC pixel clock from the VOUT CRG divider `dc8200_pix` = PLL2 / N.
   - PLL2 = 1188 MHz, so /8 = 148.5 MHz exactly and /16 = 74.25 MHz exactly.
   - SFL instead takes it from the HDMI PHY's output `hdmitx0_pixelclk`. Both paths exist in the clock tree. Details in §2.4. [C]
6. **Cache:**
   - JH7110 has **no `dma-noncoherent`** in either DT, so Linux treats the DC as coherent.
   - **But the vendor DRM driver flushes the whole L2 on every plane update** (SFL `drivers/gpu/drm/verisilicon/vs_dc.c` L949).
   - The TRM documents an **un-cacheable DDR alias at 0x4_4000_0000** ("DDR for system port", attribute "RWX N", no "C"). Details in §5.

---

## 1. Device-tree nodes and hard facts

### 1.1 Display controller DC8200

The node exists only in SFL. Source: `arch/riscv/boot/dts/starfive/jh7110.dtsi` L1526–1559. [C]

| Property | Value |
|---|---|
| compatible | `"starfive,jh7110-dc8200","verisilicon,dc8200"` (SFU fork: `"starfive,sf-dc8200"`, SFU `arch/riscv/dts/jh7110.dtsi` L1316) |
| reg[0] | `0x29400000 0x100`: "hi" = AHB top registers (IRQ, ID) |
| reg[1] | `0x29400800 0x2000`: "low". The driver writes `reg_base + reg - 0x800`, so **every DC offset below is relative to 0x2940_0000** (SFL `vs_dc_hw.c` L1176–1186) |
| reg[2] | `0x17030000 0x1000`. This is the PMU address, mapped by SFL as "mmu_base" (`vs_dc.c` L1564), only under CONFIG_VERISILICON_MMU. **Ignore it.** [U: looks like a DT copy/paste artefact] |
| interrupts | PLIC **95** |
| clocks | syscrg NOC_BUS_DISP_AXI(60), VOUT_SRC(58), VOUT_TOP_AXI(62), VOUT_TOP_AHB(61); voutcrg DC8200_PIX0(7), DC8200_PIX1(8), DC8200_AXI(4), DC8200_CORE(5), DC8200_AHB(6), DOM_VOUT_TOP_LCD(9), DC8200_PIX(1); plus `hdmitx0_pixelclk` |
| resets | syscrg VOUT_TOP_SRC(43), NOC_BUS_DISP_AXI(26); voutcrg DC8200_AXI(0), DC8200_AHB(1), DC8200_CORE(2) |

Memory map from TRM [D]:

- `0x2940_0000–0x2947_FFFF` DC8200 AHB0
- `0x2948_0000–0x294F_FFFF` DC8200 AHB1
- `0x2959_0000` U0_HDMITX (64 KiB)
- `0x295B_0000` VOUT_SYSCON
- `0x295C_0000` VOUT_CRG
- `0x295D_0000` DSI TX
- `0x295E_0000` MIPITX DPHY

ID registers:

- Revision at `0x2940_0024` reads **0x5720** and CID at `+0x30` reads **0x30E** (LY, on a Star64 board). [C]
- SFL maps rev 0x5720 to DC_REV_0: 2 panels, 6 layers (`vs_dc_hw.c` L1313–1340, L1088–1110). [C]
- ML `vs_hwdb.c` L59–66 matches 0x8200/0x5720 with any customer id. [C]

### 1.2 HDMI TX (Innosilicon)

SFL `jh7110.dtsi` L1561–1579. [C]

| Property | Value |
|---|---|
| compatible | `"starfive,jh7110-hdmi","inno,hdmi"` (SFU fork: `"starfive,inno-hdmi"`, SFU dtsi L1368) |
| reg | `0x29590000 0x4000`. **Byte registers on a 32-bit stride**: HDMI register N is at `base + N*4`, accessed as 32-bit (SFL `inno_hdmi.c` L140–149; SFU `sf_hdmi.c` L22–30) |
| interrupts | PLIC **99** |
| clocks | voutcrg HDMI_TX_SYS(17) "sysclk", HDMI_TX_MCLK(15), HDMI_TX_BCLK(16), plus `hdmitx0_pixelclk` "pclk" |
| resets | voutcrg HDMI_TX_HDMI(9) |

Board node, SFL `jh7110-starfive-visionfive-2.dtsi` L1038–1054 [C]:

- `hpd-gpio = <&sysgpio 15 GPIO_ACTIVE_HIGH>`
- `hdmi_0p9-supply = <&hdmi_0p9>`, `hdmi_1p8-supply = <&hdmi_1p8>`

### 1.3 HDMI power rails (AXP15060 PMIC)

Source: SFL VF2 dtsi L321–361. [C]

- The PMIC `x-powers,axp15060` is at **I2C5 address 0x36**. The I2C5 controller is at `0x1205_0000` (snps,designware-i2c) and runs at 100 kHz (ML `jh7110.dtsi` L724–732; ML `jh7110-common.dtsi` L194).
- `hdmi_0p9` = **ALDO5** at 0.9 V.
- `hdmi_1p8` = **ALDO3** at 1.8 V.
- Both are `regulator-boot-on`.

Enable sequence: the vendor `inno_hdmi_bind` enables 1p8, waits 100 µs, enables 0p9, waits 100 µs (SFL `inno_hdmi.c` L940–953). [C]

AXP15060 registers (ML `include/linux/mfd/axp20x.h` L222–239; `drivers/regulator/axp20x-regulator.c` L310–316, L1263–1271) [C]:

| Register | Function | Value |
|---|---|---|
| `0x11` PWR_OUT_CTRL2 | enable bits | bit2 = ALDO3, bit4 = ALDO5 |
| `0x21` ALDO3_V_CTRL | voltage, bits 4:0, 700 mV + 100 mV × N | 1.8 V → `0x0B` |
| `0x23` ALDO5_V_CTRL | same encoding | 0.9 V → `0x02` |

- [U] Whether the PMIC OTP or U-Boot leaves ALDO3/ALDO5 on is unknown.
  - The mainline DT does not declare them, so nothing in mainline turns them on or off.
  - LY saw HDMI_STATUS = 0x30 (hot-plug bit clear) on Star64 and suspected missing ALDOs. He never finished. **Read reg 0x11 first.**
- [U] U-Boot reads the board EEPROM, which suggests the I2C5 pinmux (GPIO19/20) is already set (ML common dtsi L391–394). Treat that as a guess.

### 1.4 VOUT syscon "dssctrl"

- `0x295B_0000`, size 0x90, compatible `"starfive,jh7110-dssctrl","verisilicon,dss-ctrl","syscon"` (SFL dtsi L1581–1584). [C]
- SFL writes `+0x4 |= BIT(20)` (RGB to TDA998x path) and `+0x8 |= BIT(3)` (DSI path) in `vs_dc_enable` (SFL `vs_dc.c` L702–704, and under ifdefs at L636–640).
- **Neither is needed for the on-chip HDMI.** The SFU HDMI path never touches dssctrl. [C, U]

### 1.5 VOUT CRG

| Node | Facts |
|---|---|
| `voutcrg` | `clock-controller@295c0000`, `"starfive,jh7110-voutcrg"`, size 0x10000. Parent clocks: syscrg VOUT_SRC, VOUT_TOP_AHB, VOUT_TOP_AXI, VOUT_TOP_HDMITX0_MCLK, I2STX0_BCLK, and `hdmitx0_pixelclk`. Reset: syscrg VOUT_TOP_SRC. `power-domains = <&pwrc JH7110_PD_VOUT>` (ML `jh7110.dtsi` L1265–1281). [C, mainline binding] |
| `hdmitx0_pixelclk` | A `fixed-clock` stand-in for the HDMI PHY's output, given `clock-frequency = <297000000>` (ML `jh7110.dtsi` L293–297; `jh7110-common.dtsi` L102–104). The real PHY generates it from a 24 MHz refclk (Wilczynski RFC binding, `refoclk = &xin24m`). [C] |

### 1.6 PMU (power domains)

ML `jh7110.dtsi` L1202–1207: `power-controller@17030000`, `"starfive,jh7110-pmu"`, reg 0x10000, PLIC **111**. [C, binding]

Domain ids and bits, from ML `include/dt-bindings/power/starfive,jh7110-pmu.h` and `drivers/pmdomain/starfive/jh71xx-pmu.c` L381–410 [C]:

| Domain | Id | PMU bit |
|---|---|---|
| SYSTOP | 0 | bit0 |
| CPU | 1 | bit1 |
| **GPUA** | 2 | **bit2** |
| VDEC | 3 | bit3 |
| **VOUT** | **4** | **bit4** |
| ISP | 5 | bit5 |
| VENC | 6 | bit6 |

- **There is no "DSP" domain on the JH7110 PMU.** The GPU domain is GPUA. [C]
- The AON PMU (`aon_syscon`) adds DPHY_TX bit30 and DPHY_RX bit31. Those are MIPI only, so HDMI does not need them.

### 1.7 Pinctrl for HDMI

Source: SFL VF2 dtsi L942–975 (`hdmi_pins`); signal numbers from SFL `jh7110-pinfunc.h` L40–42, L152–154, L215–218. [C]

| GPIO | Function | GPOUT | GPOEN | GPI | Pad config |
|---|---|---|---|---|---|
| 0 | HDMI_DDC_SCL | 11 | 3 | 6 | pull-up |
| 1 | HDMI_DDC_SDA | 12 | 4 | 7 | pull-up |
| 14 | HDMI_CEC_SDA | 10 | 2 | 5 | (optional) |
| 15 | HPD input | `GPOUT_LOW` | `GPOEN_DISABLE` | `GPI_SYS_HDMI_HPD` = 8 | bias-disable (external pull-up), input-enable |

- **DDC is the HDMI controller's own I2C master, muxed onto GPIO0/1.** It is not an I2C block. [C]
- Mainline `jh7110-common.dtsi` has no HDMI pinmux, so **minikern must program it**. [C]

Register encoding (ML `drivers/pinctrl/starfive/pinctrl-starfive-jh7110-sys.c` L32–35; `pinctrl-starfive-jh7110.c` L255–290) [C]:

- sys pinctrl base `0x1304_0000`.
- DOEN base `0x000` and DOUT base `0x040`: byte lane `pin%4` of word `4*(pin/4)`.
- GPI base `0x080`: signal `din` uses byte `din%4` of word `4*(din/4)`, and the value written is **`pin+2`**.
- GPIOIN at `0x118` holds the pin levels.

So for the HDMI pins:

- **HPD:** GPI reg `0x13040088` byte 0 = 15+2 = 17.
- **DDC SCL:** GPI 6 is at reg `0x13040084` byte 2, value 2.
- **DDC SDA:** GPI 7 is at reg `0x13040084` byte 3, value 3.
- **HPD level can also be read directly** from GPIOIN `0x13040118` bit 15, independent of HDMI IP power. [C]
- Remember the existing CLAUDE.md fact: 32-bit read-modify-write only.

---

## 2. Power-up sequence

### 2.1 PMU: VOUT power domain on

Base `0x1703_0000`. Source: ML `drivers/pmdomain/starfive/jh71xx-pmu.c` L17–48, L97–163. LY also reproduced it by hand. [C, plus LY]

| Offset | Register |
|---|---|
| 0x0C | SW_TURN_ON_POWER |
| 0x10 | SW_TURN_OFF_POWER |
| 0x44 | SW_ENCOURAGE |
| 0x48 | TIMER_INT_MASK |
| 0x80 | CURR_POWER_MODE |
| 0x88 | EVENT_STATUS |
| 0x8C | INT_STATUS |

Steps:

1. If `0x80` bit4 is already set, VOUT is on; stop.
2. Write `0x0C = 0x10`. Write a 1 only for the domain to turn on and 0 for all others.
3. Write `0x44 = 0xFF`. This resets the command parser.
4. Write `0x44 = 0x05`, then `0x44 = 0x50`. These are the "power on" encourage codes; power-off uses `0x0A`, `0xA0`.
5. Poll `0x80 & 0x10`. Linux allows 100 µs with a 1 µs poll interval.
   - LY read 0x3 → 0x13 on Star64, then recommends a 50 ms wait before touching VOUT.
   - SFU waits 50 ms after power-on (`sf_vop.c` L815–816).
6. Error bits are in `0x8C`: bits 3:2 = software encourage fail, bits 8:6 = p-channel fail.

[U] The TRM PMU register page URL I tried returned 404, so the offsets are from driver code. They are corroborated by two independent bare-metal and driver sources.

### 2.2 SYSCRG clocks and resets feeding VOUT

Base `0x1302_0000`. Each clock register is at `4 × id`. [C: ML `drivers/clk/starfive/clk-starfive-jh71x0.h` L11–18]

Clock register format:

- bit31 = enable (gate)
- bit30 = invert
- bits 27:24 = mux select
- bits 23:0 = divider

| Id | Offset | Name | Type and parent (ML `clk-starfive-jh7110-sys.c` L126–134) |
|---|---|---|---|
| 58 | 0xE8 | vout_src | GATE, parent pll2_out |
| 59 | 0xEC | vout_axi | DIV (max 7), parent pll2_out. No gate; leave the U-Boot or reset value |
| 60 | 0xF0 | noc_bus_disp_axi | GATE, parent vout_axi |
| 61 | 0xF4 | vout_top_ahb | GATE, parent ahb1 |
| 62 | 0xF8 | vout_top_axi | GATE, parent vout_axi |
| 63 | 0xFC | vout_top_hdmitx0_mclk | GATE, parent mclk (audio). Enabled by SFL and LY |

- LY also set bit31 at 0x28 (ahb1, id10), 0x4C (mclk_out, id19), 0x98 and 0x9C (noc_bus_cpu_axi id38, axicfg0 id39). Those are CLK_IS_CRITICAL, so they are presumably already on. [C]
- PLL2 is set to **1188 MHz** by U-Boot SPL (MU `drivers/clk/starfive/clk-jh7110-pll.c` L128, L354–355). [C]
- [U] Verify PLL2 by reading the PLL2 registers in sys_syscon `0x1303_002C`/`0x30`. FBDIV is bits 28:17 at 0x2C (ML `clk-starfive-jh7110-pll.c` L57–65).

**SYSCRG resets** (ML `drivers/reset/starfive/reset-starfive-jh7110.c` L21–25; semantics in `reset-starfive-jh71x0.c` L33–58) [C]:

- Assert registers start at `0x2F8`, four words. Reset id N lives in word `N/32`, bit `N%32`. **1 = held in reset.**
- Status registers start at `0x308`. After deassert, wait for the status bit to read **1**.

The two resets needed:

| Reset | Id | Deassert | Then poll |
|---|---|---|---|
| NOC_BUS_DISP_AXI | 26 | clear `0x2F8` bit 26 | `0x308` bit 26 == 1 |
| VOUT_TOP_SRC | 43 | clear `0x2FC` bit 11 | `0x30C` bit 11 == 1 |

- [U] **LY wrote bit 26 at 0x308**, which is the status register per ML, not 0x2F8. It probably "worked" because noc_disp was already deasserted. Use 0x2F8.
- ML also warns: "if the associated clock is gated, deasserting might otherwise hang forever." **Enable the clock before deasserting its reset.**

### 2.3 VOUT CRG clocks and resets

Base `0x295C_0000`. Same register format, at `4 × id`. Source: ML `drivers/clk/starfive/clk-starfive-jh7110-vout.c` L33–71; ids from ML `include/dt-bindings/clock/starfive,jh7110-crg.h` L279–299. [C]

| Id | Offset | Name | Type and parents |
|---|---|---|---|
| 0 | 0x00 | apb | DIV (max 8), from vout_top_ahb |
| 1 | 0x04 | dc8200_pix | DIV (max 63), from vout_src (PLL2 1188 MHz) |
| 4 | 0x10 | dc8200_axi | GATE, from vout_top_axi |
| 5 | 0x14 | dc8200_core | GATE, from vout_top_axi |
| 6 | 0x18 | dc8200_ahb | GATE, from vout_top_ahb |
| 7 | 0x1C | **dc8200_pix0** | GMUX: **mux 0 = dc8200_pix, mux 1 = hdmitx0_pixelclk** |
| 8 | 0x20 | dc8200_pix1 | GMUX, same parents |
| 9 | 0x24 | dom_vout_top_lcd | GMUX: 0 = pix0, 1 = pix1 |
| 15 | 0x3C | hdmi_tx_mclk | GATE |
| 16 | 0x40 | hdmi_tx_bclk | GATE |
| 17 | 0x44 | hdmi_tx_sys | GATE, from apb |

**VOUTCRG resets** (ML `reset-starfive-jh7110.c` L45–49; ids from `include/dt-bindings/reset/starfive,jh7110-crg.h` L198–212) [C]:

- Assert register at **0x48**, status at **0x4C**.
- Bits: 0 = DC8200_AXI, 1 = DC8200_AHB, 2 = DC8200_CORE, **9 = HDMI_TX**.
- LY wrote 0 to 0x48 to deassert everything. He also found that 0x38 did nothing. After that the VOUT CRG and DC registers read back non-zero (DC rev 0x5720).

Order used by SFU `vout_probe_resources_jh7110` (`sf_vop.c` L267–356) and SFL `dc_init`/`dc_vout_clk_enable` (`vs_dc.c` L485–509, L600–635) [C]:

1. Enable noc_disp clock (syscrg 0xF0).
2. Deassert the noc_disp reset.
3. Enable vout_src, vout_top_axi and vout_top_ahb.
4. Deassert vout_src (VOUT_TOP_SRC).
5. Enable voutcrg pix0, pix1, axi, core and ahb (and, in SFL, dom_vout_top_lcd).
6. Deassert DC8200 axi, core and ahb.
7. HDMI side (SFL `inno_hdmi.c` L162–191; SFU `sf_hdmi.c` L500–526): enable hdmi sys, mclk and bclk, then deassert HDMI_TX.

### 2.4 Where the DC pixel clock comes from

SFL `vs_dc_enable`, `vs_dc.c` L748–755 [C]:

- For display id 1 (DSI/RGB), SFL uses `dc8200_pix0` at `mode->clock`.
- **For display id 0 (HDMI), it uses `clk_set_parent(dc8200_pix0, hdmitx0_pixelclk)`**, so the DC runs from the HDMI PHY's pre-PLL pixel output.
- That means the **HDMI PHY PLL must be configured and locked before the DC panel is started.**

SFU HDMI path, `sf_vop.c` L497–505 [C]:

- `clk_set_parent(dc_pix0, "dc8200_pix0")`, then `clk_set_rate(dc8200_pix0, 148500000)`. This is the voutcrg **divider** path.
- Register form: voutcrg `0x04` divider = 8 (1188 / 8 = 148.5 MHz), and `0x1C` = enable with **mux 0**.
- For 720p, divider = 16 gives 74.25 MHz.
- [U] This decouples the DC clock from the PHY. It is the simplest first bring-up, but the DC and PHY clocks are then not phase-locked. Both derive from the same 24 MHz crystal, so they are frequency-exact, but the vendor Linux chose the PHY clock. If the TMDS output shows FIFO slips, switch to mux 1.

Register values:

- **Vendor-Linux path (PHY clock):** voutcrg `0x1C = 0x8100_0000` (enable, mux = 1 hdmitx0_pixelclk).
- **SFU path (divider clock):** `0x04 = (0x04 & ~0xFFFFFF) | 8`, then `0x1C = 0x8000_0000`.
- [U] `0x24` dom_vout_top_lcd is enabled by SFL, and its parent is left on pix0. It is probably only for the DPI/LCD pins. Enabling it is harmless.

---

## 3. Innosilicon HDMI TX (base 0x2959_0000, register N at +N×4)

### 3.1 Register map

From SFL `inno_hdmi.h` and ML `drivers/gpu/drm/bridge/inno-hdmi.c` (Rockchip, the same controller IP). [C]

**0x00 SYS_CTRL** (SFL h L30–51):

| Bit | Meaning |
|---|---|
| 6 | 1 = analog not in reset |
| 5 | 1 = digital not in reset |
| 4 | register-clock invert |
| 3 | vclk invert |
| 2 | register-clock source (1 = sys) |
| 1 | **1 = power off** |
| 0 | interrupt polarity (1 = high) |

The vendor writes **0x63** (powered down) and **0x61** (powered on).

**Video registers:**

| Register | Meaning |
|---|---|
| 0x01 VIDEO_CONTRL1 | bit0 DE source (1 = external); bits 3:1 input format |
| 0x02 VIDEO_CONTRL2 | output colour bits 7:6; input bits 5:4; input CSP bit0 |
| 0x03 | auto-CSC bit7; C0/C2 swap bit0 |
| 0x04 VIDEO_CONTRL3 | color-depth-not-indicated bit4; SOF bit3; range bit2; CSC bit0 |
| 0x05 AV_MUTE | AVMUTE clear bit7, set bit6; audio mute bit1; video black bit0 |
| 0x08 VIDEO_TIMING_CTL | VSYNC polarity bit3, HSYNC polarity bit2, interlace bit1, external video bit0 |
| 0x09/0x0A | HTOTAL low/high |
| 0x0B/0x0C | HBLANK = htotal − hdisplay |
| 0x0D/0x0E | HDELAY = htotal − hsync_start |
| 0x0F/0x10 | HDURATION = hsync_end − hsync_start |
| 0x11/0x12 | VTOTAL low/high |
| 0x13 | VBLANK |
| 0x14 | VDELAY = vtotal − vsync_start |
| 0x15 | VDURATION |

Sources: SFL h L53–132; programmed in SFL `inno_hdmi.c` L427–472.

**DDC and EDID:**

- 0x4B/0x4C DDC clock divider = `(clk >> 2) / 100 kHz`.
- 0x4D EDID segment pointer, 0x4E EDID word address, 0x4F EDID FIFO offset, 0x50 EDID FIFO data (SFL h L218–224).
- **0x52 HDCP_CTRL** bit1: **1 = HDMI, 0 = DVI** (SFL h L266–268; ML inno-hdmi.c L288–290, L781).

**Packets (infoframes):**

- 0x9C manual send, 0x9D auto send.
- 0x9F packet buffer index: **AVI = 0x06**, VSI = 0x05, AAI = 0x08.
- 0xA0+ packet bytes (ML inno-hdmi.c L246–265).

**Interrupts and status:**

- 0xC0/0xC1 INT_MASK1 and INT_STATUS1: bit2 EDID_READY, bit5 active VSYNC.
- **0xC8 HDMI_STATUS: bit7 = HOTPLUG level**, bit5 = hot-plug interrupt mask, bit1 = hot-plug interrupt (SFL h L270–285).
- 0xC9 colorbar / BIST: `0x10` = normal, `0x00` = BIST pattern, `0x04` = PHY mode (SFU `sf_hdmi.c` L386). Useful as a **self-test pattern that needs no DC.** [C, U]
- 0xCE PHY_SYNC: write 0 then 1.

**PHY / PLL at 0x1A0–0x1D3** (SFL h L365–370, L414–515; `inno_hdmi.c` L264–301). [C]

- [U] The layout matches the Rockchip "inno HDMI PHY" register set (rk3328 family, ML `drivers/phy/rockchip/phy-rockchip-inno-hdmi.c`) at offset +0x100. That is inferred and was not checked line by line.

| Register | Field |
|---|---|
| 0x1A0 | pre-PLL power-down bit0 (1 = down); vco-div-5 bit1 |
| 0x1A1 | pre-PLL prediv (bits 5:0) |
| 0x1A2 | `0xF0 \| fbdiv[11:8]` (integer mode); `0xC0 \| …` when fractional |
| 0x1A3 | fbdiv[7:0] |
| 0x1A4 | `tmds_div_a<<4 \| tmds_div_b<<2 \| tmds_div_c` |
| 0x1A5 | `pclk_div_b<<5 \| pclk_div_a` |
| 0x1A6 | `pclk_div_c<<5 \| pclk_div_d` |
| 0x1A9 | bit0 = pre-PLL lock |
| 0x1AA | post-PLL control: `0x0F` while configuring, `0x0E` = post-div enabled and running, `0x02` = no post-div. bit0 = power-down |
| 0x1AB | post-PLL prediv |
| 0x1AC | post-PLL fbdiv |
| 0x1AD | post-PLL postdiv |
| 0x1AF | bit0 = post-PLL lock |
| 0x1B0 | `\|= 0x04`, set before PLL configuration (SFL calls it "bandgap enable") |
| 0x1B2 | TMDS driver enable: `0x8F` = clock, D2, D1 and D0 enabled |
| 0x1B4 | LDO enable: `0x07` |
| 0x1BE | serializer enable: `0x71` (SFL) or `0x70` (SFU) |
| 0x1BF/0x1C0 | eye-diagram tweak per VIC: VIC 16 → 0x02/0x22; VIC 4 → 0x00/0x00 (SFL L399–425) |
| 0x1CC | `= 0x0F`, set before PLL configuration |
| 0x1D1–0x1D3 | fractional divider 23:16, 15:8, 7:0 |

### 3.2 PLL settings (24 MHz reference)

The SFU `#else` branch is the non-27M path, i.e. 24 MHz. The SFL table and SFU produce **identical** bytes for both modes. [C]

**Mode 1: 1080p60, VIC 16, 148.5 MHz**

- SFL pre-PLL entry `{148500000, 148500000, prediv=1, fbdiv=99, tmds a/b/c = 1/1/1, pclk a/b/c/d = 1/2/2/2, vco5 = 0, frac = 0}` (`inno_hdmi.c` L101).
- SFL post-PLL entry `{148500000, prediv=1, fbdiv=20, postdiv=1, post_div_en=3}` (L131).
- SFU `inno_hdmi_config_1920x1080p60` matches (`sf_hdmi.c` L265–282).

| Register | Value |
|---|---|
| 0x1A0 | 0x01 |
| 0x1AA | 0x0F |
| 0x1A1 | 0x01 |
| 0x1A2 | 0xF0 |
| 0x1A3 | 0x63 |
| 0x1A4 | 0x15 |
| 0x1A5 | 0x41 |
| 0x1A6 | 0x42 |
| 0x1AB | 0x01 |
| 0x1AC | 0x14 |
| 0x1AD | 0x01 |
| 0x1AA | 0x0E |
| 0x1A0 | 0x00 |

**Mode 2: 720p60, VIC 4, 74.25 MHz**

- SFL pre-PLL entry `{74250000, …, 1, 99, 1, 2, 2, 1, 2, 3, 4, 0, 0}` (L77); post-PLL `{74250000, 1, 20, 1, 3, 3}` (L128).
- SFU `config_1280x720p60` (`sf_hdmi.c` L220–237) matches.

| Register | Value |
|---|---|
| 0x1A0 | 0x01 |
| 0x1AA | 0x0F |
| 0x1A1 | 0x01 |
| 0x1A2 | 0xF0 |
| 0x1A3 | 0x63 |
| 0x1A4 | 0x1A |
| 0x1A5 | 0x41 |
| 0x1A6 | 0x64 |
| 0x1AB | 0x01 |
| 0x1AC | 0x14 |
| 0x1AD | 0x01 |
| 0x1AA | 0x0E |
| 0x1A0 | 0x00 |

### 3.3 Init sequence for a fixed mode

Source: SFL `inno_hdmi_setup`, `inno_hdmi.c` L475–511. SFU `inno_hdmi_enable` (`sf_hdmi.c` L444–457) is the same minus the timing registers. [C]

1. Enable the HDMI clocks and deassert HDMI_TX (§2.3).
2. `0x1B0 |= 0x04`, then `0x1CC = 0x0F`.
3. Write the PLL table (§3.2), ending with `0x1A0 = 0x00`.
4. Spin until `0x1A9 & 1`, then until `0x1AF & 1`. The vendor loops are unbounded; **add a timeout**.
5. `0x1B4 = 0x07` (LDO), then `0x1BE = 0x71` (serializer).
6. Eye-diagram tweak: `0x1BF`/`0x1C0`.
7. `0x00 = 0x63` (PHY/controller power down).
8. Timing registers `0x09`–`0x15`, then `0x08 = 0x01 | (+H ? 0x04 : 0) | (+V ? 0x08 : 0)`. CEA 1080p and 720p are both +H +V, so **0x0D**.
9. `0x00 = 0x61` (power on).
10. `0x1B2 = 0x8F` (TMDS drivers on). SFU then waits 50 ms.
11. `0xCE = 0x00`, then `0xCE = 0x01` (data sync).

Timing register values [C, arithmetic]:

| Register | 1080p (htot 2200, hact 1920, hss 2008, hse 2052, vtot 1125, vact 1080, vss 1084, vse 1089) | 720p (1650 / 1280 / 1390 / 1430 ; 750 / 720 / 725 / 730) |
|---|---|---|
| 0x09 / 0x0A | 0x98 / 0x08 | 0x72 / 0x06 |
| 0x0B / 0x0C | 0x18 / 0x01 (280) | 0x72 / 0x01 (370) |
| 0x0D / 0x0E | 0xC0 / 0x00 (192) | 0x04 / 0x01 (260) |
| 0x0F / 0x10 | 0x2C / 0x00 (44) | 0x28 / 0x00 (40) |
| 0x11 / 0x12 | 0x65 / 0x04 | 0xEE / 0x02 |
| 0x13 | 0x2D (45) | 0x1E (30) |
| 0x14 | 0x29 (41) | 0x19 (25) |
| 0x15 | 0x05 | 0x05 |

**HDMI vs DVI, and the minimal AVI infoframe** [U]:

- **Neither the vendor Linux nor U-Boot writes 0x52 (HDMI/DVI) or a valid AVI infoframe.**
- SFU `inno_hdmi_tx_ctrl` (`sf_hdmi.c` L376–387) writes `0x9F = 0x06`, then `0xA0 = 0x82`, `0xA2 = 0x0D`, `0xA3 = 0x00`, `0xA4 = 0x00`, `0xA5 = 0x08`, `0xA6 = 0x70`, `0xA7 = VIC`, and finally `0xC9 = 0x10`.
  - That buffer skips the version byte at 0xA1 and writes a zero checksum, so it is malformed. It is unclear whether `inno_hdmi_tx_ctrl` is even reached for the logo path; it is, from `phy_param_config`.
- So the shipping StarFive stack effectively relies on the controller's reset default for 0x52. Monitors accept it.
- To be correct, use the Rockchip method (ML `inno-hdmi.c` L584–604, L770–790):
  1. `0x05` set audio-mute and video-black.
  2. `0x52 = 0x02` (HDMI mode). Leave it 0 for DVI, which needs no infoframe.
  3. `0x9F = 0x06`, then write the packed infoframe to `0xA0 + i`, for 17 bytes:
     - header: `0x82, 0x02, 0x0D, checksum`
     - PB1 = `0x00` (RGB, no scan info)
     - PB2 = `0x08` (aspect: same as picture), or set picture aspect 16:9 as `0x28`
     - PB3 = `0x00`
     - PB4 = VIC (16 or 4)
     - rest 0
     - checksum = 256 − sum of all other bytes, mod 256.
  4. Unmute `0x05`.
  - Also program `0x01 = 0x01` (DE external, RGB444 SDR) and `0x02 = 0x00` (8-bit RGB in and out) as ML does (L644–651). [C]
- **Recommendation: start in DVI mode (0x52 = 0), with no infoframe.**

### 3.4 HPD

- **Controller:** `HDMI_STATUS` (0xC8) bit7 = hot-plug level. This needs the HDMI clocks, reset and power. SFU probe returns connected when the bit is set (`sf_hdmi.c` L528); SFL `connector_detect` does the same (L570–586). [C]
- **GPIO alternative:** sysgpio pin 15, active high, with an external pull-up. The vendor uses this pin as the HPD interrupt source (SFL `inno_hdmi.c` L966–976). [C]

### 3.5 DDC / EDID over the controller's master

Sources: SFL `inno_hdmi.c` L321–333 (init), L753–835 (transfer); ML `inno-hdmi.c` same logic. [C]

Init:

- `0x4B`/`0x4C` = `(rate >> 2) / 100000`.
  - SFL uses a placeholder `tmds_rate = 51.2 MHz`, giving **128** (`0x80`, `0x00`) (L979–981).
  - [U] The divisor's input clock is presumably hdmi_tx_sys (apb) and is not documented. 128 is the value used on this SoC.
- `0xC0 = 0` (mask off), `0xC1 = 0x04` (clear EDID_READY).

Reading a 128-byte EDID block:

1. `0xC0 = 0x04` (unmask EDID_READY), then `0xC1 = 0x04` (clear it).
2. `0x4F = 0x00` (FIFO offset).
3. `0x4E = word_addr`: 0x00 for block 0, 0x80 for block 1.
4. `0x4D = segment`. **This write starts the transfer.**
5. Wait for `0xC1 & 0x04` (the vendor allows a 100 ms timeout).
6. Clear it: `0xC1 = 0x04`.
7. Read 128 bytes from `0x50`.
8. `0xC0 = 0`.

- [U] Polling STATUS1 instead of taking the IRQ should work, since the vendor sets the mask bit and then waits on the status.
- The DDC pins must be muxed (§1.7) and the 5 V / HPD rails present.

---

## 4. DC8200 register map for one primary plane

All offsets are from **0x2940_0000** and accessed as 32-bit. Names follow ML `drivers/gpu/drm/verisilicon/*_regs.h`; values and bits are cross-checked against SFL `vs_dc_hw.h`/`vs_dc_hw.c` and the working SFU write sequence (`sf_vop.c` L512–559). `(n)` is the display or panel index; HDMI is **display 0** (see §4.5). [C]

### 4.1 Top block (the "hi" window)

From ML `vs_dc_top_regs.h` L14–25:

| Offset | Register |
|---|---|
| 0x0000 | TOP_RST |
| **0x0010** | **IRQ_ACK**: read returns and clears pending; bit n = VSYNC of display n |
| **0x0014** | **IRQ_EN**: bit n = VSYNC enable. SFU writes `0xC0001FFF` |
| 0x0020 | CHIP_MODEL (0x8200) |
| 0x0024 | CHIP_REV (0x5720) |
| 0x0030 | CUSTOMER_ID (0x30E on Star64, per LY) |

- vblank IRQ = **PLIC 95**.
- Underflow: `FB_CONFIG(0)` bit5 (SFL `vs_dc_hw.c` L1514–1517). [C]

### 4.2 Primary plane (framebuffer 0 = display 0's primary)

From ML `vs_primary_plane_regs.h`; SFL h L28–55.

| Offset | Register | Contents |
|---|---|---|
| 0x1400 | FB_ADDRESS(n) | 32-bit physical address. Written as `lower_32_bits` (ML), with a DMA mask of 32 in ML (`vs_dc.c` L68); SFL uses a 40-bit mask. [U] **Keep the FB below 4 GiB, i.e. in 0x4000_0000–0xFFFF_FFFF** |
| 0x1408 | FB_STRIDE(n) | bytes per line. 1920×4 = **0x1E00**; 1280×4 = **0x1400**. SFL `pitch_alignment` = 32 bytes |
| 0x1518 | FB_CONFIG(n) | see layout below |
| 0x1810 | FB_SIZE(n) | `w \| h<<15` (`VSDC_MAKE_PLANE_SIZE`, ML `vs_plane.h` L19). 1080p = **0x021C0780**; 720p = **0x01680500** |
| 0x24D8 | FB_TOP_LEFT(n) | `x \| y<<15`; set 0 |
| 0x24E0 | FB_BOTTOM_RIGHT(n) | `x \| y<<15`; equal to the size value above (SFU writes 0x021C0780) |
| 0x1CC0 | FB_CONFIG_EX(n) | see layout below |
| 0x2510 | FB_BLEND_CONFIG(n) | ML writes BLEND_DISABLE = BIT(1) = **0x2**. SFU writes 0x2042 or 0xA9A3, which are opaque values |
| 0x1CE8 | FB_WATER_MARK | SFU uses 0 for 1080p HDMI on a newer board and 0xF61 in another path. [U] |
| 0x1A28 | CLK_GATTING | SFU writes 0 at enable and 0xE07 at teardown |
| 0x1A38 | QOS | SFU writes 0xE8 |

**FB_CONFIG (0x1518)** fields:

| Bits | Field |
|---|---|
| 31:26 | FORMAT. **X8R8G8B8 = 5** (`0x1400_0000`); A8R8G8B8 = 6, which SFU uses (`0x1800_0000`) |
| 24:23 | SWIZZLE. 0 = ARGB, which is DRM XRGB8888: little-endian word 0xXXRRGGBB (SFL `vs_dc.c` L136) |
| 25 | UV swizzle |
| 21:17 | tile mode (0 = linear) |
| 16:14 | YUV colour space |
| 13:11 | rotation |
| 8 | clear-enable |
| 5 | underflow (read) |

**FB_CONFIG_EX (0x1CC0)** fields:

| Bit(s) | Field |
|---|---|
| 13 | **FB_EN** |
| **12** | **COMMIT**. SFL calls it the shadow-register enable |
| 18:16 | zpos |
| 19 | DISPLAY_ID |
| 6 | RGB path (SFL sets it for RGB formats: `vs_dc_hw.c` L1798–1802) |
| 5 | degamma |
| 0 | ROI |

**Overlays:**

- Overlay 0 is at `0x1540` (config) and `0x2540` (CONFIG_EX). Each later overlay adds +4 to both.
- SFU writes the config with `0x0006_0000` (format 6, enable bit24 = 0), so **overlays stay disabled**. Leaving them at reset value 0 also means disabled.

**Cursor:**

- `0x1468` CURSOR_CONFIG: bits 1:0 = format, 0 = disabled.
- SFU writes `0x1484 = 0` (cursor clock gating) and `0x24E8` (CONFIG_EX).

### 4.3 Display (CRTC) timing

From ML `vs_crtc_regs.h`; SFL `setup_display` L1873–1968.

| Offset | Register | Contents |
|---|---|---|
| 0x1430 | DISP_HSIZE(n) | `hactive \| htotal<<16` |
| 0x1438 | DISP_HSYNC(n) | `hsync_start \| hsync_end<<15 \| BIT30 (enable) \| BIT31 if sync is NEGATIVE` |
| 0x1440 | DISP_VSIZE(n) | `vactive \| vtotal<<16` |
| 0x1448 | DISP_VSYNC(n) | same layout as HSYNC |
| 0x1450 | DISP_CURRENT_LOCATION(n) | read-only scan position, a cheap "is it running" check |
| 0x1410 | DITHER_CONFIG(n) | 0 = off |
| 0x147C / 0x1480 | DISP_IRQ_STA / DISP_IRQ_EN | — |

Computed values. 1080p matches the SFU hard-coded writes `0x08980780`, `0x440207D8`, `0x04650438`, `0x4220843C` (`sf_vop.c` L541–544). [C]

| Mode | 0x1430 | 0x1438 | 0x1440 | 0x1448 |
|---|---|---|---|---|
| 1080p60 | 0x08980780 | 0x440207D8 | 0x04650438 | 0x4220843C |
| 720p60 | 0x06720500 | 0x42CB056E | 0x02EE02D0 | 0x416D02D5 |

### 4.4 Panel / output enable

From ML `vs_bridge_regs.h`; ML `vs_bridge.c` L144–216.

| Offset | Register | Contents |
|---|---|---|
| 0x1418 | DISP_PANEL_CONFIG(n) | bit0 DE_EN, bit1 DE_POL, bit4 DAT_EN, bit5 DAT_POL, bit8 CLK_EN, bit9 CLK_POL, **bit12 RUNNING**, bit13 gamma, bit16 YUV. SFU and SFL use **0x1111** (SFL init writes 0x111, then sets bit12) |
| 0x14B8 | DISP_DPI_CONFIG(n) | bits 2:0 format, RGB888 = **5** |
| 0x1CD0 | DISP_DP_CONFIG(n) | bits 2:0 format (RGB888 = 2), **bit3 DP_EN**, bits 7:4 YUV format |
| 0x1CCC | DISP_PANEL_START | **bit n = RUNNING for display n**, bit2 = ?, bit3 = MULTI_DISP_SYNC |
| 0x2518 | DISP_PANEL_CONFIG_EX(n) | bit0 = "COMMIT" (ML) |
| 0x1528 | FB_BG_COLOR(n) | — |

DP_CONFIG value for HDMI:

- SFL drives HDMI (TMDS encoder) as **OUT_DP**, writing `DP_CONFIG(0) = 2 | BIT(3) = 0x0A` (`vs_dc_hw.c` L1970–2026; `vs_dc.c` L757–761).
- SFU writes `0x1CD0 = 0xD2`, which has no bit3, together with `DPI_CONFIG = 5`.
- **[U] These conflict. Try the SFL value 0x0A first.**

**Commit semantics conflict** [U]:

- SFL `vs_dc_commit` (`vs_dc.c` L1344–1361) and `dc_hw_enable_shadow_register` (`vs_dc_hw.c` L1519–1548):
  - Before writing: clear `FB_CONFIG_EX` bit12 and **set** `PANEL_CONFIG_EX` bit0.
  - After writing: set `FB_CONFIG_EX` bit12 and **clear** `PANEL_CONFIG_EX` bit0.
- SFU matches this: `0x1CC0 = 0x2000` early and `0x3000` at the end; `0x2518 = 1` early and `= 0` at the end.
- ML instead *sets* `PANEL_CONFIG_EX` bit0 as "COMMIT" after enabling (`vs_bridge.c` L172), and sets `FB_CONFIG_EX` bit12 after each plane update.
- **Follow the SFU/SFL sequence. It was validated on JH7110 silicon; ML was validated on TH1520.**
- For a framebuffer address flip at runtime: write 0x1400, then pulse `FB_CONFIG_EX` bit12 (clear, then set). The address latches at vsync. [U]

### 4.5 Which output is HDMI

- **HDMI = display/panel 0**:
  - SFU programs only the panel-0 registers (offset +0) and `PANEL_START` bit0 (`0x1CCC = 1`) for HDMI (`sf_vop.c` L559).
  - SFL uses the `hdmitx0_pixelclk` parent only when `display.id != 1` (`vs_dc.c` L748–755). [C]
- The DC to HDMI path needs no dssctrl mux (§1.4). [U]

### 4.6 Minimal DC program (SFU order, simplified)

Sources: `sf_vop.c` L512–559 and ML. [C, U]

1. **Verify the DC is alive.** Read `0x24`; it must be 0x5720, otherwise power or clocks are missing.
2. **Mark the panel update pending.** `0x2518 = 1`.
3. **Primary plane:**
   - `0x1CC0 = 0x2000` (FB_EN, display 0, commit clear).
   - `0x24D8 = 0`.
   - `0x24E0 = size`, `0x1810 = size`.
   - `0x1400 = fb_pa`, `0x1408 = stride`.
   - `0x1CE8 = 0`.
   - `0x2510 = 0x2`.
   - `0x1518 = 0x1400_0000`.
4. **Commit the plane.** `0x1CC0 = 0x3000`.
5. **Clock gating off.** `0x1A28 = 0`.
6. **Timing.** `0x1430`, `0x1438`, `0x1440`, `0x1448` per §4.3.
7. **Output format.** `0x14B0 = 0` (GENERAL_CONFIG), `0x1CD0 = 0x0A` (or SFU's 0xD2), `0x14B8 = 5`.
8. **Panel enable.** `0x1418 = 0x1111`, `0x1410 = 0`.
9. **Commit the panel.** `0x2518 = 0`.
10. **Start display 0.** `0x1CCC = 0x1`.
11. **Optional vsync interrupt.** `0x14 |= 1`, then service PLIC 95 by reading `0x10`.

[U] SFU also writes the debug-counter register `0x14D0` and the cursor registers; those are not needed.

---

## 5. Cache coherency

### Is the DC DMA-coherent on JH7110?

- **Mainline DT:** the JH7110 `soc` node has **no `dma-noncoherent`** (ML `jh7110.dtsi` L355–360). By contrast JH7100 has `dma-noncoherent` (ML `jh7100.dtsi` L153) and an uncached DMA window via `dma-ranges` at `0x10_7A00_0000` (ML `jh7100-common.dtsi` L63–67).
  - On RISC-V a device is coherent unless marked otherwise, so **mainline Linux treats every JH7110 device as coherent**. [C]
- **Vendor SFL:** it also marks no `dma-noncoherent`, and marks only xrp, jpu, vpu_dec, vpu_enc and e24 explicitly `dma-coherent` (SFL dtsi L737, 1079, 1100, 1125, 1641).
  - **Yet the vendor DRM driver calls `sifive_ccache_flush_entire()` on every plane update** (SFL `vs_dc.c` L949), and `sifive_ccache_flush_range()` for cursor buffers (L1273).
  - SFU calls `video_set_flush_dcache(dev, 1)` (`sf_vop.c` L846).
- **Conclusion [U]:** treat the DC scanout as **not coherent** with dirty L2 lines. The vendor that knows the silicon flushes.

### How Linux flushes (Flush64)

- ccache at **`0x0201_0000`**, size 0x4000; compatible `"starfive,jh7110-ccache","sifive,ccache0","cache"`.
- 2 MiB, 64-byte lines, 2048 sets (ML `jh7110.dtsi` L373–383).
- **FLUSH64 is at offset 0x200**; FLUSH32 at 0x240 (ML `drivers/cache/sifive_ccache.c` L47–48).

Per-line flush loop (ML L147–166) [C]:

1. `fence` (mb).
2. For each 64-byte line in [start, end): 64-bit store of the **physical** line address to `0x0201_0200`.
3. `fence`.

It writes back and invalidates. ML registers it as the `wback`, `inv` and `wback_inv` operation for `starfive,jh7110-ccache` (L127–128, L168–172, L323–326).

- Documented in the SiFive U74-MC Core Complex manual "Flush64" register. [D in SiFive's manual; not re-fetched here]
- 1920×1080×4 = 8.29 MB = 129,600 line writes per full flush. Flushing only dirty rectangles is worthwhile.

Vendor "flush entire" (SFL `drivers/soc/sifive/sifive_ccache.c` L162–205) [C]:

- Uses the **L2 zero device at `0x0A00_0000`** (32 MiB) with per-master WayMask registers. The vendor ccache node lists regs `0x0201_0000` (0x4000), `0x0800_0000` (LIM, 32 MiB) and `0x0A00_0000` (zero device, 32 MiB) (SFL dtsi L400–411).
- That is more complex and needs way-masking. **Prefer the Flush64 range loop.**

### Is there an uncached DRAM alias?

**Yes, per the TRM memory map [D]:**

| Range | Size | Description | Attribute |
|---|---|---|---|
| `0x00_4000_0000–0x02_3FFF_FFFF` | 8 GB | "DDR for memory port" | "RWX C A N" |
| **`0x04_4000_0000–0x06_3FFF_FFFF`** | 8 GB | "DDR for system port" | "**RWX N**" (no C) |

- The TRM page has no attribute legend. "C" is very likely cacheable, matching SiFive core-complex conventions where the System Port is uncached.
- So **PA 0x4_4000_0000 + (x − 0x4000_0000)** is an uncached CPU view of DRAM x. [D address; U semantics]
- **No Linux tree uses it on JH7110.** The SFL ccache driver supports an `uncached-offset` DT property, but no JH7110 DTS sets it (grep of SFL dts: none). [C]

How minikern could use it [U]:

- Map the framebuffer's CPU view in Sv39 at the alias, at 1 GiB or 2 MiB granularity. The U74 has no Svpbmt, so the alias is the only way to get uncached CPU pages.
- Hand the DC the **normal** PA, below 4 GiB.
- Flush64 the range **once** before switching to the alias, so no stale dirty L2 lines can later be evicted over it.
- Risks:
  - The OpenSBI PMP might not grant S-mode access to `0x4_4000_0000`. Test with one read.
  - Uncached writes are slow. Write-combining is unlikely.
- Alternative: keep a cached framebuffer and Flush64 the dirty lines after each frame. That is the vendor's approach, and it is known to work.

---

## 6. Suggested bring-up order for minikern (synthesis)

Synthesis of the above. [C, U]

0. **Board rails.** Over I2C5 (DesignWare at `0x1205_0000`), read AXP15060 `0x36` reg `0x11`. If bit2 or bit4 is clear:
   - set ALDO3 `0x21 = 0x0B`, enable bit2, wait 100 µs;
   - set ALDO5 `0x23 = 0x02`, enable bit4, wait 100 µs.
1. **Pinmux** GPIO0/1 (DDC) and GPIO15 (HPD) per §1.7.
   - Plugged-monitor sanity check: GPIOIN `0x13040118` bit15 == 1.
2. **PMU VOUT on** (§2.1), then wait 50 ms.
3. **syscrg** (§2.2):
   - gate on 0xF0;
   - deassert reset 26;
   - gate on 0xE8, 0xF8, 0xF4, 0xFC;
   - deassert reset 43.
4. **voutcrg** (§2.3, §2.4):
   - gate on 0x10, 0x14, 0x18, 0x20, 0x24, 0x3C, 0x40, 0x44;
   - 0x1C = enable plus the mux choice. Divider path: `0x04` div = 8 (1080p) or 16 (720p), mux 0.
   - Then 0x48 bits 0, 1, 2, 9 = 0, and poll 0x4C.
   - Check: `0x2940_0024` == 0x5720.
5. **HDMI** §3.3 steps 2–11. In DVI mode you may skip infoframes. Read 0xC8 bit7 for HPD and the EDID per §3.5.
6. **DC** §4.6 with the framebuffer below 4 GiB, Flush64-ed or written through the system-port alias.
7. **Self-tests:**
   - DC running: `0x1450` (current location) changes.
   - HDMI path without the DC: write `0xC9 = 0x00` for the BIST colour bar [U].
   - DC underflow: `0x1518` bit5.

## 7. Open questions and risks

1. ALDO3/ALDO5 state at boot is unknown. Measure it.
2. HDMI pixel-clock source: the divider (SFU) or the PHY (SFL). Try the divider first.
3. DP_CONFIG `0x0A` (SFL) versus `0xD2` (SFU).
4. `PANEL_CONFIG_EX` bit0 polarity: SFL/SFU versus ML.
5. No valid AVI infoframe exists in either StarFive stack. 0x52 HDMI/DVI is unset by both. Default to DVI.
6. The DDC clock divider base clock is undocumented. 128 is the SoC-tested value.
7. The system-port alias semantics and its PMP accessibility are unverified.
8. **The TRM pages for PMU, VOUT CRG, DC8200 and HDMI were not fetched**; the TRM PMU URL I tried returned 404. Register facts here come from code, cross-checked across 2–4 independent implementations (SFL, SFU, ML, LY) wherever possible.

---

# Part 2 — Acceleration

Research date 2026-10-31. Scope: what can accelerate 2D/3D for a from-scratch Linux-ABI kernel
(musl + clang 21 userspace, no root on the build host). Confidence tags: **[verified]** = read in
a primary source or measured from an artifact during this research; **[secondary]** = forum/press/
search-snippet; **[uncertain]** = inference or unconfirmed — measure on the board before relying on it.

---

## 0. Verdict in one table

| Option | What it buys | Feasible for minikern? | Effort |
|---|---|---|---|
| DC8200 planes (primary + cursor, later overlays, scaler, CSC) | Zero-copy composition, hardware cursor, YUV scaling | **Yes — the biggest, cheapest win.** Already on the plan (scanout). | M (primary+cursor), +M (overlays/scaler) |
| Damage-tracked CPU blits + cache-line flush only for dirty rows | Turns 8 MB/frame into KB/frame | **Yes, required anyway** | S |
| dw-axi-dmac mem-to-mem copies/fills | CPU offload (not speed) | Yes, hardware supports memcpy; fill via fixed-source is plausible but unverified | M |
| llvmpipe / lavapipe / softpipe (Mesa, CPU) | Standards GL/GLES/Vulkan | Yes (runs on riscv64 musl today in Alpine); slow — fine for simple UI, not for games | M (KMS dumb-buffer ABI + run Alpine binaries) |
| PowerVR BXE-4-32 via own `drm/pvr`-ABI driver + Mesa `imagination` Vulkan + Zink | Real 3D | **Technically possible, practically XL**: not mainline-supported, not Mesa-supported, reverse-engineered power-up, mediocre measured perf | XL (≈12–20 kLOC) |
| Vendor closed DDK (1.19) | GLES 3.2 | **No**: closed, glibc-linked, needs vendor `pvrsrvkm` Linux module ABI | — |
| VPU (WAVE511/420L) / JPU (CODAJ12) | Video/JPEG decode | Out of scope for "2D accel"; no mainline driver; vendor-only | L each |
| CPU vector | — | **None**: U74 has no V | — |

---

## 1. Hardware inventory

### 1.1 CPU — SiFive U74 ×4 @ up to 1.5 GHz
- RV64GC, 32 KiB L1 I + 32 KiB L1 D per core, 2 MiB shared L2 ("ccache" at `0x2010000`, 64-byte
  lines, 2048 sets) **[verified: mainline jh7110.dtsi; CNX spec sheet]**.
- **No vector extension** — confirmed: the mainline tree's `riscv,isa-extensions` for the U74 harts
  lists `i m a f d c zba zbb zicntr zicsr zifencei zihpm` and no `v` **[verified: torvalds/linux
  master `arch/riscv/boot/dts/starfive/jh7110.dtsi`]**.
- **New fact relevant to the project's CLAUDE.md §2 ("`-mcpu=sifive-u74` = exactly rv64gc")**:
  mainline declares **Zba + Zbb** on these harts (`riscv,isa = "rv64imafdc_zba_zbb"`). clang's
  `sifive-u74` model omits them. `-march=rv64gc_zba_zbb` is available if verified on silicon (trap-
  probe one `sh1add`/`orc.b`). Small win for string/addr code; does nothing for bulk bandwidth.
- Also on die: S7 monitor core (RV64IMAC, no MMU) and an E24 RV32IMFC real-time core
  **[verified: CNX]** — neither is a useful blitter.

### 1.2 GPU — Imagination PowerVR B-Series **BXE-4-32 MC1**
- **BVNC `36.50.54.182`**, Vulkan deviceID `0x36054182` **[verified: Mesa `src/imagination/common/
  device_info/bxe-4-32.h`; vulkaninfo in domibel bring-up commit]**.
- From Mesa's device info: 1 cluster, 1 raster pipe, 16×16 tiles, ASTC, compute, FBCDC algorithm 50,
  max MSAA 4×, multicore-capable (MC1) **[verified]**.
- **Firmware processor is MIPS32r2**: `rogue_36.50.54.182_v1.fw` is a 131 072-byte "ELF 32-bit LSB
  executable, MIPS32 rel2", strings `firmware/mips/rgxfw_utils_mips.c`, `rgxfw_kernel_ccb.c`
  **[verified: downloaded and inspected; sha256 59127d3a…7abb, commit 8a58f818 of
  imagination/linux-firmware]**. So a driver needs the MIPS FW-wrapper/MMU path, not META or RISC-V.
- Marketing: OpenGL ES 3.2, Vulkan 1.2, OpenCL 1.2 **[secondary: CNX]**. Clock (commonly quoted up
  to 600 MHz) **[uncertain]**.
- MMIO `0x1800_0000` (1 MiB) + a second window `0x0130_c000`; IRQ 82; power domain
  `JH7110_PD_GPUA` (PMU `pwrc@17030000`); clocks syscrg `GPU_CORE`, `GPU_SYS_CLK`,
  `NOC_BUS_GPU_AXI`, `GPU_CORE_CLK`, `GPU_APB`, `GPU_RTC_TOGGLE`; resets `GPU_APB`, `GPU_DOMA`;
  marked `dma-noncoherent` **[verified: domibel `riscv: dts: starfive: add PowerVR BXE-4-32 GPU node
  to jh7110.dtsi` — out-of-tree]**.

### 1.3 "2D acceleration": there is no blitter
There is **no 2D GPU / blitter / G2D-style engine** on JH7110 that I could find in the datasheet
feature lists or any driver tree **[verified absence in CNX spec + datasheet display chapter;
absence claims are inherently uncertain]**. The 2D acceleration that exists is in the display
controller and the generic DMA engine.

### 1.4 Display controller — Verisilicon DC8200
Per the JH7110 datasheet display chapter and StarFive's SDK display guide **[verified]**:
- 2 display pipelines / 2 panels; **6 image layers shared between the 2 panels**; video/graphic
  (primary) layers, **overlay 0–3**, **hardware cursor** (ARGB8888 and mask formats).
- **Overlay 1 and 3 have no scaling, rotation or line buffers**; the others do.
- **Scaling**: H 3/5-tap, V 3-tap, programmable filter, 15.16 fixed-point factor.
- **Alpha blending with 8 Porter-Duff modes**; RGB and YUV inputs, **linear and tiled** buffers;
  **CSC BT.709/BT.2020**; per-display gamma + dither LUTs; write-back (debug only).
- Up to 4K2K (HDMI 2.0 4Kp30), default 1080p.
- Mainline driver `drivers/gpu/drm/verisilicon` (landed for Linux 7.1, TH1520 first) exposes only
  **primary + cursor** planes, ~2.2 kLOC, formats RGB444/1555/565/8888/2101010 **[verified: master
  tree file list + grep; Phoronix]**. JH7110 enablement (vout subsystem, Inno HDMI PHY = RK3328 PHY at
  +0x100, HDMI bridge) is the Samsung/Wilczynski series, **v6 posted 2026-10-29, not merged**
  **[verified: ratatoskr dri-devel archive]**.
- Implication: the hardware can **compose up to ~5 surfaces + cursor with alpha, scale video and
  convert YUV at scanout time with zero CPU or memory copies** — the most effective "2D accel" here.

### 1.5 DMA engine — Synopsys DW AXI DMAC (`starfive,jh7110-axi-dma`)
`dma-controller@16050000`: **4 channels**, 1 master, 64-bit data width (`snps,data-width = <3>`),
AXI burst 16, block size 65 536 items per block (= 512 KiB per LLI block at 64-bit), IRQ 73, clocks
`stgcrg DMA1P_AXI/AHB` **[verified: mainline jh7110.dtsi]**. Linux driver advertises **DMA_MEMCPY**
(only) **[verified: dw-axi-dmac-platform.c]**. Linked-list (LLI) multi-block mode makes a 2-D blit
one LLI per row. Fill: the controller has fixed-address source mode (SINC = no-change) in the DW AXI
DMAC register model, which yields a fill from an 8-byte pattern **[uncertain — databook semantics,
not exercised by Linux, not tested on JH7110]**.

### 1.6 VPU / JPU
Chips&Media **WAVE511** decoder (H.264/H.265 up to 4K60), **WAVE420L** encoder (H.265 1080p30),
**CODAJ12** JPEG enc/dec **[secondary: CNX + StarFive soft_3rdpart repo]**. No mainline JH7110
driver found **[uncertain]**; vendor drivers + firmware only. Irrelevant to 2D/3D UI; only for video
playback later.

### 1.7 Cache coherency (matters for scanout and any DMA/GPU work)
- The ccache driver registers **non-standard cache ops** for `starfive,jh7110-ccache`: flush is one
  MMIO write per 64-byte line to `ccache + 0x200` (FLUSH64) **[verified: drivers/cache/sifive_ccache.c]**.
- The GPU node is marked `dma-noncoherent` out-of-tree, and the bring-up community attributes much of
  the GPU slowness to non-coherent DMA (uncached BOs, software flushes) **[secondary: domibel issue #3]**.
- minikern itself already does NVMe/virtio/GMAC DMA **without** maintenance and it works
  (`src/drivers/dwmac.c` prints the tree's coherency claim). Whether the **DC8200** read master is
  coherent with L2 is **[uncertain]** — must be tested: draw a pattern, don't flush, look at HDMI.
  If not coherent, a full-frame flush is 129 600 MMIO writes (1080p32); its cost is **unmeasured**.

---

## 2. PowerVR state of the art (open-source stack)

### 2.1 Kernel: mainline `drm/imagination`
- Supported cores (Linux docs v7.3-rc1 and `pvr_device.c` on master): **AXE-1-16M (33.15.11.3),
  BXM-4-64 (36.52.104.182), BXS-4-64 (36.53.104.796)**. **BXE-4-32 is not listed**; any other BVNC
  returns `PVR_GPU_UNKNOWN` (gated behind `exp_hw_support`) **[verified]**.
- Out-of-tree JH7110 support exists: domibel's `powervr_on_jh7110_visionfive2_v7.3-rc5` branch =
  v7.3-rc5 + **12 patches** (DT node, extra clocks/second reset, a BXE-4-32 power-up via
  `RGX_CR_POWER_EVENT` whose bit layout is literally named `GUESS_*` in the patch, i.e. reverse-
  engineered, plus 7 robustness fixes for FW teardown/MMU/refcount leaks found on this core)
  **[verified: GitHub commit list and patches]**. Not submitted/merged upstream as far as found.

### 2.2 Firmware
- File `powervr/rogue_36.50.54.182_v1.fw`, FW v1.1 build 6976702 OS. **Not in upstream
  linux-firmware** (its WHENCE lists only the three supported cores) — it is only in
  **gitlab.freedesktop.org/imagination/linux-firmware** (branch `powervr`) **[verified]**.
- License `LICENSE.powervr`: binary redistribution permitted unmodified; **no reverse engineering**;
  only for Imagination-licensed hardware; the **patent grant applies only when combined with an OSI-
  approved open-source OS** **[verified: license text]**. → minikern must itself be under an OSI
  licence (the ARM predecessor is MIT) and the blob is third-party code — a §2 deviation from "no
  third-party code" would have to be recorded.

### 2.3 Mesa
- Mesa PowerVR (`imagination`) Vulkan driver docs list **BXE-4-32 36.50.54.182 under
  "Unsupported (not under active development)"**; conformant Vulkan 1.2 only on BXM-4-64/BXS-4-64;
  others need `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` **[verified: docs.mesa3d.org/drivers/powervr]**.
  Device info for BXE-4-32 added in Mesa 25.3 **[secondary: Phoronix]**; works in practice with
  Mesa 26.1/26.3-rc1 **[verified: domibel README/commit]**.
- **OpenGL only via Zink** (GL 3.2 core/compat, GLES 3.1 on Vulkan 1.2) **[verified: domibel]**.
- Mesa also has an `imagination-srv` backend (meson option) that talks to the **vendor services
  kernel module** instead of `drm/pvr` **[verified: meson.options]**.
- Measured on VF2 with that stack (vkmark, 1080p, direct KMS): clear 469, cube 159, shading 92,
  texture 84, desktop **22**, effect2d **9** FPS; glmark2-es2 jellyfish 5 FPS; glxgears ~30 FPS under
  labwc/Zink; WebGL aquarium ~2 FPS vs ~32 FPS on the vendor stack **[verified: domibel README]**.
  BO churn + non-coherent DMA identified as the bottleneck; patched Mesa took vkmark 118→302 at
  800×600 **[secondary: issue #3]**.

### 2.4 Userspace ABI Mesa expects (what minikern would have to implement)
From Mesa `src/imagination/vulkan/winsys/powervr/*.c` **[verified by grep]**:
- `DRM_IOCTL_PVR_*`: `DEV_QUERY`, `CREATE_BO`, `GET_BO_MMAP_OFFSET` (+ `mmap` on the render node),
  `CREATE/DESTROY_VM_CONTEXT`, `VM_MAP/UNMAP`, `CREATE/DESTROY_CONTEXT`, `CREATE/DESTROY_FREE_LIST`,
  `CREATE/DESTROY_HWRT_DATASET`, `SUBMIT_JOBS` (14 ioctls, uapi `include/uapi/drm/pvr_drm.h`,
  1 295 lines, **licence `(GPL-2.0 WITH syscall-note) OR MIT`**).
- Generic DRM: `GEM_CLOSE`, `SYNCOBJ_CREATE/DESTROY/…WAIT/TRANSFER` (timeline syncobjs used by
  Mesa's common Vulkan runtime), `PRIME_HANDLE_TO_FD/FD_TO_HANDLE` (dma-buf fds), `VERSION`,
  plus device discovery as libdrm does it (`/dev/dri/renderD128`, `/sys/dev/char/226:128/device/…`).
- WSI to the screen additionally needs **KMS** on the DC8200 node (ADDFB2 from an imported dma-buf,
  atomic/page-flip, vblank events) — the same KMS ABI llvmpipe would need.

### 2.5 Effort proxy: size of mainline `drm/imagination`
- **~35 000 lines** of C/H in 80 files on master, of which ~5 500 are firmware-interface headers
  (`pvr_rogue_fwif*.h`) and ~15 000 the core (`pvr_mmu.c` 2 647, `pvr_drv.c` 1 539, `pvr_fw.c`
  1 516, `pvr_queue.c` 1 512, `pvr_vm.c` 1 198, `pvr_job.c` 783, `pvr_power.c` 726, `pvr_ccb.c` 692,
  `pvr_free_list.c` 633, `pvr_fw_meta.c` 577, `pvr_hwrt.c` 546, `pvr_context.c` 489, `pvr_gem.c` 429,
  `pvr_sync.c` 289, `pvr_fw_mips.c` 199 …) **[verified: counted]**. The driver is **dual GPL-2.0 OR
  MIT** — usable as documentation, though the project rule forbids copying Linux code regardless.
- It stands on DRM core services minikern lacks: GEM/shmem, `drm_gpuvm`, `drm_sched`, dma-fence,
  syncobj, dma-buf, runtime PM.
- What a from-scratch driver must do: PMU power-on of `PD_GPUA`; 6 clocks + 2 resets; read BVNC from
  `CORE_ID`; SLC init; the guessed `POWER_EVENT` sequence; load and relocate the MIPS ELF FW into
  GPU-visible memory; build the **MIPS FW MMU wrapper** and the **GPU MMU 3-level page tables** (per-VM
  contexts, kernel/FW heap layout Mesa assumes); FW init structures (`RGXFWIF_SYSINIT`/OSINIT/…);
  **kernel CCB + FW CCB** rings, per-context **client CCBs** for geometry/fragment/compute/transfer
  queues; free-list (parameter buffer) growth on FW request; HWRT datasets; FW-signalled fences over
  IRQ 82; HW recovery; cache maintenance for every non-coherent BO.
- Estimate: **12–20 kLOC of new C** for a minimal but robust subset, plus a DRM-subset (GEM handles,
  mmap offsets, syncobj, dma-buf fds) of perhaps 3–5 kLOC → **XL**, months, with a reverse-engineered
  power-up and firmware whose behaviour can only be debugged through its own trace buffer.

### 2.6 Vendor DDK route
StarFive ships DDK 1.19 (`1.19@6345021`, GLES 3.2) as closed binaries in `soft_3rdpart`
(OpenEmbedded marks the license CLOSED); needs the vendor `pvrsrvkm` kernel module and its bridge
ioctl ABI, glibc-linked userspace, separate xorg/wayland builds **[secondary]**. For minikern:
reimplementing an undocumented proprietary ioctl bridge with glibc-only blobs on musl — **not viable**.

---

## 3. Mesa on musl, on a non-Linux kernel with a Linux ABI

- **Precedent**: Mesa's `meson.build` lists **`managarm`** (a from-scratch microkernel OS with a
  Linux-compatible DRM ABI) among systems with KMS/DRM **[verified]**. Because minikern *is* the Linux
  ABI, Mesa should be configured simply as `host_machine.system() == 'linux'` (target
  `riscv64-linux-musl`).
- **Musl + riscv64 Mesa is a solved build**: Alpine ships **Mesa 26.2.6** for riscv64 with
  `llvmpipe`, `zink`, `softpipe`, Vulkan `swrast` (lavapipe) and LLVM 23; it carries a
  `riscv64-tls.patch` (TLS dialect detection) **[verified: aports main/mesa/APKBUILD]**. Alpine does
  **not** build `imagination` for riscv64 (only aarch64), but the option exists for any arch.
- **Shortcut worth taking first**: since minikern runs stock Linux binaries, **Alpine riscv64 musl
  packages (mesa-dri-gallium, mesa-egl, mesa-gbm, mesa-vulkan-swrast, llvm-libs) can be run unchanged**
  — no Mesa build at all, which sidesteps the no-root/host-tool problem. Building it yourself needs
  meson ≥ 1.4, Python 3 + mako + PyYAML + packaging (+ ply/cparser for some drivers), bison/flex,
  glslang (Vulkan drivers), libdrm ≥ 2.4.109 (higher for some drivers), expat, zlib, LLVM for
  llvmpipe. (Python here is a host build tool for a C project, not Go tooling.)
- **Kernel features Mesa needs** beyond what Go already exercises: `mmap` with `PROT_EXEC` / W^X
  remapping (llvmpipe JIT), many pthreads + futex, `sched_getaffinity`, `memfd_create` (lavapipe/WSI),
  `/proc/self/*` bits, `poll`/`eventfd`; for a display path: `/dev/dri/card0` with **KMS dumb
  buffers** (`MODE_CREATE_DUMB`, `MODE_MAP_DUMB`, `MODE_ADDFB2`, `SETCRTC`/atomic, page-flip events);
  for a GPU: render node, dma-buf fds, syncobj (above).
- **llvmpipe on riscv64**: works only through the **ORCJIT** backend (MCJIT failed with "Relocation
  type not implemented"); merged upstream ~2024; on riscv64 ORCJIT is forced on **[verified:
  meson.options text; secondary for the merge date]**. Reported **glmark2 score ≈ 10** on a JH7110
  (PineTab-V) **[secondary: MR !26018 via search snippet]**. Debian reported crashes with 24.2 on
  riscv64 **[secondary]**; the domibel vulkaninfo shows `llvmpipe (LLVM 21.1.8, 128 bits)` enumerating
  on VF2 under Mesa 26.1 **[verified]**. With no vector unit the "128 bits" lanes are scalarised.
  **Usable for simple 2D-in-GL UIs at reduced resolution; not for anything animated at 1080p.**
- Note: OSMesa was removed from recent Mesa **[uncertain on exact version, ≈25.x]**; headless GL is
  EGL surfaceless/pbuffer + readback, or GBM on the KMS node.

---

## 4. Software 2D on the U74

### 4.1 Memory bandwidth (what is known)
- DRAM: 32-bit LPDDR4, 2 133 MT/s working speed (2 800 max) **[verified: U-Boot docs / datasheet]**
  → theoretical ≈ 8.5 GB/s.
- Measured: tinymembench "~800 MB/s, surprisingly even across tests" on an 8 GB VF2
  **[secondary: magazin Mehatronika]**; on Star64 (same SoC) copy ≈ half of read/write
  **[secondary: RVspace thread, figures only in a GIF]**. No reliable published memset/memcpy split
  was found → **measure it**: an 8 MiB `memset` and `memcpy` loop in a ktest gives the number in
  seconds.

### 4.2 What a 1080p frame costs (1920×1080×4 = 8 294 400 B)

| Sustained rate | Full-frame fill | Full-frame copy (blit) | 60 Hz budget used by one full copy |
|---|---|---|---|
| 0.8 GB/s (reported) | 10.4 ms | 10.4 ms | 62 % of 16.7 ms |
| 1.6 GB/s | 5.2 ms | 5.2 ms | 31 % |
| 3.2 GB/s | 2.6 ms | 2.6 ms | 16 % |

Add scanout's own read traffic: 8.3 MB × 60 = **~0.5 GB/s** of DRAM bandwidth permanently, and, if the
DC8200 master proves non-coherent, a 129 600-write flush per full frame (cost unknown). Conclusion:
**full-frame CPU redraw at 1080p60 is not realistic on this memory system; damage-rectangle
redraw is mandatory**, and a cursor must be the hardware cursor plane, never a software sprite.
RGB565 halves every number above if the UI tolerates it. Four harts can split fills, but they share
one 32-bit DRAM channel — expect little scaling beyond 2 harts **[uncertain]**.

### 4.3 dw-axi-dmac as a blitter
Usable for: full-frame/large-rect copies (one LLI per row), page-flip-free double-buffer copies,
moving image data while the CPU does something else. It will **not** beat a well-written CPU loop on
raw bandwidth — the DRAM is the limit — the win is that 4 harts stay free. It is a non-coherent-
master question again (flush before, invalidate after) **[uncertain]**. Fill via fixed-source
**[uncertain]**. No raster ops, no alpha, no format conversion: compositing still belongs to the
DC8200 planes.

---

## 5. Recommended roadmap (ordered by win per effort)

1. **(S) Measure first.** ktest: 8 MiB memset/memcpy per hart and with 2/4 harts; cost of a FLUSH64
   sweep over 8 MiB; DC8200 coherency test (draw without flush, check HDMI). Also trap-probe Zba/Zbb.
   These four numbers decide everything below.
2. **(M) DC8200 scanout = primary plane + hardware cursor** (the existing plan), behind a Linux
   **fbdev** (`/dev/fb0`: `FBIOGET_VSCREENINFO`, `FBIOPAN_DISPLAY` double-buffering via a 2×-height
   buffer, mmap) — the cheapest ABI that stock software (fbterm, SDL2 fbcon/kmsdrm-less, Go programs)
   uses. Cursor plane is free "acceleration".
3. **(S) Damage tracking + dirty-row flush** in whatever userspace draws, and page-flip (pan) instead
   of copy. This is the largest *software* win (frame cost becomes proportional to what changed).
4. **(M) Minimal KMS** on `/dev/dri/card0`: dumb buffers, ADDFB2, SETCRTC + page-flip events,
   cursor ioctls. Unlocks **Mesa kms_swrast/llvmpipe via GBM/EGL** and SDL2/kmsdrm, weston-pixman,
   etc., and is the prerequisite for any GPU path.
5. **(M) Run Alpine riscv64 musl Mesa unchanged** (llvmpipe/softpipe GL, lavapipe Vulkan). Gives
   standards GL/GLES for simple apps today, slow (glmark2 ≈ 10).
6. **(M→L) DC8200 overlays + scaler + CSC** exposed as KMS planes (beyond what mainline does):
   hardware composition of up to ~5 layers, YUV video scaled at scanout. Real 2D acceleration with
   zero copies; also what makes VPU output useful later.
7. **(M) dw-axi-dmac memcpy offload** for large blits, if step 1 shows CPU time (not DRAM) is what
   the UI is short of.
8. **(XL, optional / research) PowerVR BXE-4-32**: own `drm/pvr`-ABI driver (12–20 kLOC + DRM
   subset), MIPS firmware from Imagination's fork (record the third-party-blob deviation; OSI-licence
   condition), Mesa built with `-Dvulkan-drivers=imagination` + Zink, accepting "unsupported, non-
   conformant" status and the measured ceiling (desktop scene 22 FPS at 1080p, glmark2 jellyfish
   5 FPS). Prerequisites 4 + dma-buf + syncobj. Do this only as a deliberate project in itself;
   follow the domibel branch and upstream drm/imagination for register sequences and FW-interface
   layouts (dual MIT — readable as specification).
9. **Out of reach / not worth it**: vendor DDK 1.19 (closed, glibc, proprietary ioctl bridge);
   CPU SIMD (none exists); VPU/JPU for UI work.

---

## Sources
- Mesa PowerVR driver docs — https://docs.mesa3d.org/drivers/powervr.html
- Linux drm/imagination docs (v7.3-rc1) — https://kernel.org/doc/html/v7.3-rc1/gpu/imagination/index.html
- Mainline driver source counted from torvalds/linux master `drivers/gpu/drm/imagination/`,
  `include/uapi/drm/pvr_drm.h`, `drivers/gpu/drm/verisilicon/`, `drivers/cache/sifive_ccache.c`,
  `drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c`, `arch/riscv/boot/dts/starfive/jh7110.dtsi`
- linux-firmware WHENCE (upstream mirror) — https://gitlab.com/kernel-firmware/linux-firmware
- Imagination firmware fork — https://gitlab.freedesktop.org/imagination/linux-firmware (branch `powervr`, commit 8a58f818)
- domibel bring-up — https://github.com/domibel/visionfive2_gpu_bringup ; issue #3 https://github.com/domibel/visionfive2_gpu_bringup/issues/3 ;
  kernel branch https://github.com/domibel/linux/tree/powervr_on_jh7110_visionfive2_v7.3-rc5
- Mesa source: `meson.build`, `meson.options`, `src/imagination/vulkan/winsys/powervr/`, `src/imagination/common/device_info/bxe-4-32.h` — https://gitlab.freedesktop.org/mesa/mesa
- Alpine Mesa APKBUILD — https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/main/mesa/APKBUILD
- Phoronix: Mesa PVR more GPUs — https://phoronix.com/news/PowerVR-Mesa-More-GPUs ; DC8200 for 7.1 — https://www.phoronix.com/news/Linux-7.1-DC8200-Coreboot-FB
- JH7110 display series (Wilczynski, v3→v6) — https://ratatoskr.run/linux-arm-kernel/2026/09/17514319/t
- JH7110 datasheet, display controller — https://doc-en.rvspace.org/JH7110/Datasheet/JH7110_DS/display_controller.html ;
  SDK DC8200 block — https://doc-en.rvspace.org/VisionFive2/DG_Display/JH7110_SDK/block_diagram_display.html
- CNX JH7110 spec — https://www.cnx-software.com/2022/08/29/starfive-jh7110-risc-v-processor-specifications/
- llvmpipe ORCJIT riscv — https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/26018 , Debian #1058759 https://lists.debian.org/debian-x/2023/12/msg00063.html
- Memory bandwidth — https://magazinmehatronika.com/en/visionfive-2-review/ , https://forum.rvspace.org/t/star64-memory-benchmark/3285
- Vendor DDK — https://github.com/starfive-tech/VisionFive2/releases/tag/VF2_v3.1.5 , https://layers.openembedded.org/layerindex/recipe/333647
