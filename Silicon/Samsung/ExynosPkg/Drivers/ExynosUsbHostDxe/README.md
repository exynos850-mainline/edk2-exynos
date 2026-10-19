# ExynosUsbHostDxe – USB OTG host bring-up for the Galaxy A14 4G (SM-A145F)

**Status: stage 1, syntax-checked against MdePkg headers, NOT run on hardware.**

What it does, in order (all values taken from the A14 Samsung kernel source and
your live device tree):

1. PMU `0x11860704` bit0: release USBDRD PHY isolation.
2. PHY `0x135d0000`: Samsung `phy_exynos_usb_v3p1_enable()` for main version
   0x300 (host tune values from the live DT: tx_pre_emp 3, tx_vref 0xb,
   rx_sqrx 7, compdis 7, tx_res 3, tx_rise 3), then `config_host_mode()`.
3. DWC3 `0x13600000`: soft reset, `GCTL.PRTCAPDIR = host`.
4. VBUS: S2MU106 charger at I2C `0x3d` on the controller at `0x13830000`;
   Samsung's `s2mu106_charger_otg_control(1)` register order (no charger attached).
5. Dumps xHCI CAPLENGTH/HCIVERSION/USBSTS/PORTSC to the console for 15 s just
   before boot (ReadyToBoot), and to DEBUG.

## Enable it (not in any DSC/FDF by default)

`Platform/Samsung/exynos850/a145f.dsc`, `[Components]`:

    Silicon/Samsung/ExynosPkg/Drivers/ExynosUsbHostDxe/ExynosUsbHostDxe.inf

`Platform/Samsung/exynos850/exynos850.fdf`, next to the other DXE drivers:

    INF Silicon/Samsung/ExynosPkg/Drivers/ExynosUsbHostDxe/ExynosUsbHostDxe.inf

Diagnostic only (no 5 V on VBUS): change `-DEXYNOS_USB_VBUS=1` to `0` in the .inf.

## Read the result

Plug an OTG adapter + a device, boot, photograph the log shown before the menu.

* `DWC3 GSNPSID = 0x5533....` -> link is clocked and alive. Anything else
  (or a crash) means the HSI clock gate / USB power domain is off and needs a
  CMU_HSI + power-domain step first.
* `S2MU106 not answering` -> I2C block 0x13830000 is clock/pinmux gated.
* `xHCI PORTSC1 bit0 (CCS) = 1` after VBUS up -> a device is seen: stage 2
  (hand the controller to MdeModulePkg `XhciDxe` through a PciIo shim) is next.

## Known gaps (stage 2+)

* CMU_HSI clock gates and the HSI power domain are assumed left on by S-boot.
* `vdd33_usb` (s2mpu12 LDO via ACPM) is assumed on.
* The USB-C PD chip (`s2mu106-usbpd`) must present Rp/DFP for a device to be
  powered; not driven yet.
* The xHCI hand-off to `XhciDxe` (PciIo shim) is not written yet.
