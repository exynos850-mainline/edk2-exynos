/*
 * ExynosUsbHostDxe: Exynos850 (SM-A145F) USB OTG host bring-up, STAGE 1.
 *
 * Everything here is transcribed from Samsung's A14 kernel source
 * (phy-exynos-usb3p1.c, dwc3-exynos*.c, s2mu106_charger.c) and the live device
 * tree of the phone. It has NOT been run on hardware yet. It is deliberately
 * defensive: every step is logged and the register dump at the end tells us
 * what the stock bootloader left behind.
 *
 *  1. PMU: release USBDRD PHY isolation (0x11860704 bit0).
 *  2. PHY (0x135d0000): UTMI/HS PHY power-up, "v3p1 / main version 0x300".
 *  3. DWC3 (0x13600000): soft reset, GCTL.PRTCAPDIR = host.
 *  4. S2MU106 charger (I2C 0x3d on the controller at 0x13830000): switch the
 *     OTG boost (VBUS 5 V) on, Samsung's s2mu106_charger_otg_control() order.
 *  5. Dump xHCI capability registers so we can see if the host core is alive.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

#ifndef EXYNOS_USB_VBUS
#define EXYNOS_USB_VBUS 1
#endif

/* ---- Base addresses (live device tree) ---------------------------------- */
#define PMU_BASE          0x11860000UL
#define PMU_USBDRD_PHY    (PMU_BASE + 0x704)     /* pmu_offset = 0x704 */
#define PMU_PHY_ENABLE    BIT0

#define USBPHY_BASE       0x135D0000UL           /* phy@135d0000 */
#define USBCTRL_BASE      0x13600000UL           /* usb@13600000 (xHCI/DWC3) */
#define I2C_BASE          0x13830000UL           /* i2c@13830000 holds s2mu106@3d */
#define S2MU106_ADDR      0x3D
#define GPP0_BASE        0x139B0020UL

/* ---- USB PHY control registers (phy-exynos-usb3p1-reg.h) ---------------- */
#define PHY_CTRL_VER      0x00
#define PHY_LINK_CTRL     0x04
#define   LINKCTRL_BUS_FILTER_BYPASS(x) (((x) & 0xF) << 4)
#define   LINKCTRL_FORCE_QACT           BIT8
#define   LINKCTRL_DIS_QACT_BVALID      BIT9
#define   LINKCTRL_DIS_QACT_VBUS_VALID  BIT10
#define   LINKCTRL_DIS_QACT_ID0         BIT11
#define   LINKCTRL_DIS_QACT_LINKGATE    BIT12
#define PHY_LINK_PORT     0x08
#define   LINKPORT_OCD_U3               BIT3
#define   LINKPORT_OCD_U2               BIT2
#define PHY_CLKRST        0x20
#define   CLKRST_PORT_RST               BIT1
#define   CLKRST_PHY30_RST_SEL          BIT2
#define   CLKRST_PHY30_SW_RST           BIT3
#define   CLKRST_PHY20_RST_SEL          BIT12
#define   CLKRST_PHY20_SW_RST           BIT13
#define PHY_UTMI          0x50
#define   UTMI_FORCE_SLEEP              BIT0
#define   UTMI_FORCE_SUSPEND            BIT1
#define   UTMI_DM_PULLDOWN              BIT2
#define   UTMI_DP_PULLDOWN              BIT3
#define   UTMI_FORCE_BVALID             BIT4
#define   UTMI_FORCE_VBUSVALID          BIT5
#define PHY_HSP           0x54
#define   HSP_COMMONONN                 BIT8
#define   HSP_EN_UTMISUSPEND            BIT9
#define   HSP_VBUSVLDEXT                BIT12
#define   HSP_VBUSVLDEXTSEL             BIT13
#define PHY_HSP_TUNE      0x58
#define PHY_HSP_TEST      0x5C
#define   HSP_TEST_SIDDQ                BIT24

/* HS tune values for HOST mode (hs_tune_valueN second cell in the live DT). */
#define TUNE_TXVREF       0xB   /* tx_vref   bits 31:28 */
#define TUNE_TXRISE       0x3   /* tx_rise   bits 25:24 */
#define TUNE_TXRES        0x3   /* tx_res    bits 22:21 */
#define TUNE_TXPREEMP     0x3   /* tx_pre_emp bits 19:18 */
#define TUNE_SQRX         0x7   /* rx_sqrx   bits 10:8  */
#define TUNE_COMPDIS      0x7   /* compdis   bits 2:0   */

/* ---- DWC3 registers ------------------------------------------------------ */
#define DWC3_GLOBALS      0xC100
#define DWC3_GCTL         (DWC3_GLOBALS + 0x10)
#define   GCTL_PRTCAPDIR_MASK           (3 << 12)
#define   GCTL_PRTCAP_HOST              (1 << 12)
#define   GCTL_CORESOFTRESET            BIT11
#define DWC3_GSNPSID      (DWC3_GLOBALS + 0x20)
#define DWC3_GUSB2PHYCFG  0xC200
#define   GUSB2PHYCFG_PHYSOFTRST        BIT31
#define   GUSB2PHYCFG_SUSPHY            BIT6
#define DWC3_GUSB3PIPECTL 0xC2C0
#define   GUSB3PIPECTL_PHYSOFTRST       BIT31
#define   GUSB3PIPECTL_SUSPHY           BIT17
#define DWC3_DCTL         0xC704
#define   DCTL_CSFTRST                  BIT30

/* ---- Samsung/S3C IIC registers ------------------------------------------ */
#define IICCON            0x00
#define   IICCON_ACKEN                  BIT7
#define   IICCON_IRQEN                  BIT5
#define   IICCON_IRQPEND                BIT4
#define IICSTAT           0x04
#define   IICSTAT_MASTER_TX             (3 << 6)
#define   IICSTAT_MASTER_RX             (2 << 6)
#define   IICSTAT_BUSY                  BIT5   /* write 1 = START, 0 = STOP */
#define   IICSTAT_TXRXEN                BIT4
#define   IICSTAT_ARBITR                BIT3
#define   IICSTAT_NACK                  BIT0   /* last received bit: 1 = no ACK */
#define IICDS             0x0C

/* ---- S2MU106 registers (s2mu106_charger.h) ------------------------------ */
#define S2MU106_CHG_CTRL0 0x18
#define S2MU106_CHG_CTRL3 0x1B
#define S2MU106_CHG_CTRL9 0x21

STATIC CHAR8 mLog[4096];
STATIC UINTN mLogLen;

STATIC VOID Say (IN CONST CHAR8 *Fmt, ...)
{
  VA_LIST V;
  CHAR8   Line[160];

  VA_START (V, Fmt);
  AsciiVSPrint (Line, sizeof (Line), Fmt, V);
  VA_END (V);
  DEBUG ((DEBUG_ERROR, "ExynosUsbHost: %a\n", Line));
  if (mLogLen + AsciiStrLen (Line) + 2 < sizeof (mLog)) {
    AsciiStrCpyS (mLog + mLogLen, sizeof (mLog) - mLogLen, Line);
    mLogLen += AsciiStrLen (Line);
    mLog[mLogLen++] = '\n';
    mLog[mLogLen]   = 0;
  }
}

STATIC UINT32 Rd (UINTN Base, UINTN Off) { return MmioRead32 (Base + Off); }
STATIC VOID   Wr (UINTN Base, UINTN Off, UINT32 V) { MmioWrite32 (Base + Off, V); }
STATIC VOID   Upd (UINTN Base, UINTN Off, UINT32 Set, UINT32 Clr)
{
  MmioWrite32 (Base + Off, (MmioRead32 (Base + Off) & ~Clr) | Set);
}

/* ------------------------------------------------------------------------ */
/* Polled S3C-style I2C master                                              */
/* ------------------------------------------------------------------------ */
STATIC BOOLEAN I2cWaitIrq (VOID)
{
  UINTN T;

  for (T = 0; T < 20000; T++) {          /* ~20 ms */
    if (Rd (I2C_BASE, IICCON) & IICCON_IRQPEND) {
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  return FALSE;
}

STATIC VOID I2cResume (UINT32 AckEn)
{
  UINT32 C = Rd (I2C_BASE, IICCON);

  C &= ~(IICCON_IRQPEND | IICCON_ACKEN);
  C |= IICCON_IRQEN | AckEn;
  Wr (I2C_BASE, IICCON, C);               /* writing IRQPEND=0 resumes the bus */
}

STATIC VOID I2cStop (UINT32 Mode)
{
  Wr (I2C_BASE, IICSTAT, Mode | IICSTAT_TXRXEN);   /* BUSY=0 -> STOP */
  I2cResume (0);
  MicroSecondDelay (200);
}

/* Write Len bytes (Buf[0] is the register address). */
STATIC BOOLEAN I2cWrite (UINT8 Addr, CONST UINT8 *Buf, UINTN Len, BOOLEAN Stop)
{
  UINTN   I;
  BOOLEAN Ok = FALSE;

  Wr (I2C_BASE, IICDS, (UINT32)(Addr << 1));
  Wr (I2C_BASE, IICSTAT, IICSTAT_MASTER_TX | IICSTAT_TXRXEN | IICSTAT_BUSY);
  if (!I2cWaitIrq () || (Rd (I2C_BASE, IICSTAT) & IICSTAT_NACK)) {
    goto Out;
  }
  for (I = 0; I < Len; I++) {
    Wr (I2C_BASE, IICDS, Buf[I]);
    I2cResume (IICCON_ACKEN);
    if (!I2cWaitIrq () || (Rd (I2C_BASE, IICSTAT) & IICSTAT_NACK)) {
      goto Out;
    }
  }
  Ok = TRUE;
Out:
  if (Stop || !Ok) {
    I2cStop (IICSTAT_MASTER_TX);
  }
  return Ok;
}

STATIC BOOLEAN I2cReadByte (UINT8 Addr, UINT8 *Out)
{
  BOOLEAN Ok = FALSE;

  Wr (I2C_BASE, IICDS, (UINT32)((Addr << 1) | 1));
  Wr (I2C_BASE, IICSTAT, IICSTAT_MASTER_RX | IICSTAT_TXRXEN | IICSTAT_BUSY);
  if (!I2cWaitIrq () || (Rd (I2C_BASE, IICSTAT) & IICSTAT_NACK)) {
    goto Out;
  }
  I2cResume (0);                           /* single byte: NACK it */
  if (!I2cWaitIrq ()) {
    goto Out;
  }
  *Out = (UINT8)Rd (I2C_BASE, IICDS);
  Ok = TRUE;
Out:
  I2cStop (IICSTAT_MASTER_RX);
  return Ok;
}

/* The controller is never set up by this driver otherwise: program the
 * prescaler (PCLK/512/8) and enable, and log what the boot loader left behind. */
STATIC VOID I2cInit (VOID)
{
  Say ("GPP0 before: CON=0x%08x DAT=0x%08x PUD=0x%08x DRV=0x%08x",
       Rd (GPP0_BASE, 0x00), Rd (GPP0_BASE, 0x04), Rd (GPP0_BASE, 0x08), Rd (GPP0_BASE, 0x0C));
  Upd (GPP0_BASE, 0x00, 0x22, 0xFF);
  Upd (GPP0_BASE, 0x08, 0, 0xF);
  Upd (GPP0_BASE, 0x0C, 0, 0xF);
  Say ("GPP0 after:  CON=0x%08x PUD=0x%08x", Rd (GPP0_BASE, 0x00), Rd (GPP0_BASE, 0x08));
  Say ("I2C at entry: CON=0x%08x STAT=0x%08x LC=0x%08x",
       Rd (I2C_BASE, IICCON), Rd (I2C_BASE, IICSTAT), Rd (I2C_BASE, 0x10));
  Wr (I2C_BASE, IICSTAT, IICSTAT_TXRXEN);
  Wr (I2C_BASE, IICCON, IICCON_ACKEN | IICCON_IRQEN | BIT6 | 0x7);
  MicroSecondDelay (100);
}

STATIC BOOLEAN ChgRead (UINT8 Reg, UINT8 *V)
{
  return I2cWrite (S2MU106_ADDR, &Reg, 1, TRUE) && I2cReadByte (S2MU106_ADDR, V);
}

STATIC BOOLEAN ChgWrite (UINT8 Reg, UINT8 V)
{
  UINT8 B[2] = { Reg, V };

  return I2cWrite (S2MU106_ADDR, B, 2, TRUE);
}

STATIC BOOLEAN ChgUpdate (UINT8 Reg, UINT8 Val, UINT8 Mask)
{
  UINT8 Old;

  if (!ChgRead (Reg, &Old)) {
    return FALSE;
  }
  return ChgWrite (Reg, (UINT8)((Val & Mask) | (Old & ~Mask)));
}

/*
 * s2mu106_charger_otg_control(enable) with no wired charger present
 * (REG_MODE_BUCK off), including regmode_vote(REG_MODE_OTG).
 */
STATIC BOOLEAN VbusOn (VOID)
{
  UINT8 Mode, Chk;

  I2cInit ();
  if (!ChgRead (S2MU106_CHG_CTRL0, &Mode)) {
    Say ("I2C after fail: CON=0x%08x STAT=0x%08x DS=0x%08x",
         Rd (I2C_BASE, IICCON), Rd (I2C_BASE, IICSTAT), Rd (I2C_BASE, IICDS));
    Say ("S2MU106 not answering on I2C 0x%x (bus 0x%lx): clock/pinmux gated?", S2MU106_ADDR, I2C_BASE);
    return FALSE;
  }
  Say ("S2MU106 CHG_CTRL0 = 0x%02x before OTG", Mode);

  ChgUpdate (S2MU106_CHG_CTRL3, 0x2 << 2, 0x3 << 2);   /* OTG OCP 1.2 A      */
  ChgUpdate (S2MU106_CHG_CTRL3, 0x20, 0x30);           /* OTG/TX sw always on */
  ChgUpdate (0x3B, 0x00, 0x0C);                        /* input sw current sense off */
  MicroSecondDelay (30000);
  ChgUpdate (S2MU106_CHG_CTRL9, 0x00, 0x10);           /* QBAT on despite BAT OCP */
  MicroSecondDelay (10000);

  /* regmode_vote(OTG): mode = OTG | CHG, BUCK off. */
  Mode = (UINT8)(((Mode & 0xF) | BIT2 | BIT1) & ~BIT0);
  ChgUpdate (0x3A, 0x01, 0x03);                        /* auto async */
  ChgUpdate (S2MU106_CHG_CTRL0, Mode, 0xF);
  /* regmode_vote() final branch (OTG, no BUCK): "prevent OTG OCP default". */
  ChgWrite (0x39, 0x55);
  MicroSecondDelay (20000);

  ChgUpdate (0x94, 0x0C, 0x0C);                        /* OTG fault debounce 15 ms */
  ChgUpdate (S2MU106_CHG_CTRL3, 0x10, 0x30);           /* OTG/TX switches default */
  ChgUpdate (0x3B, 0x04, 0x0C);                        /* input sw current sense on */
  MicroSecondDelay (20000);

  if (ChgRead (S2MU106_CHG_CTRL0, &Chk)) {
    Say ("S2MU106 CHG_CTRL0 = 0x%02x after OTG (want OTG bit2 set, BUCK bit0 clear)", Chk);
    return (Chk & BIT2) != 0;
  }
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* USB2 PHY bring-up: phy_exynos_usb_v3p1_enable() for main version 0x300   */
/* ------------------------------------------------------------------------ */
STATIC VOID PhyEnableHost (VOID)
{
  UINT32 V;

  Say ("PHY CTRL_VER = 0x%08x", Rd (USBPHY_BASE, PHY_CTRL_VER));

  /* exynos_cal_usbphy_q_ch(enable) */
  Upd (USBPHY_BASE, PHY_LINK_CTRL,
       LINKCTRL_DIS_QACT_ID0 | LINKCTRL_DIS_QACT_VBUS_VALID |
       LINKCTRL_DIS_QACT_BVALID | LINKCTRL_DIS_QACT_LINKGATE,
       LINKCTRL_FORCE_QACT);
  MicroSecondDelay (500);
  Upd (USBPHY_BASE, PHY_LINK_CTRL, LINKCTRL_FORCE_QACT, 0);
  MicroSecondDelay (500);

  /* phy_sw_rst_high */
  Upd (USBPHY_BASE, PHY_CLKRST,
       CLKRST_PHY20_SW_RST | CLKRST_PHY20_RST_SEL |
       CLKRST_PHY30_SW_RST | CLKRST_PHY30_RST_SEL, 0);

  Upd (USBPHY_BASE, PHY_UTMI, 0,
       UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
  Upd (USBPHY_BASE, PHY_HSP, HSP_EN_UTMISUSPEND | HSP_COMMONONN, 0);  /* common_block_disable */
  MicroSecondDelay (100);

  /* link_vbus_filter_en(false) + device-mode defaults, cleared again for host below */
  Upd (USBPHY_BASE, PHY_LINK_CTRL, LINKCTRL_BUS_FILTER_BYPASS (0xF), 0);
  Upd (USBPHY_BASE, PHY_UTMI, UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID, 0);
  Upd (USBPHY_BASE, PHY_HSP, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT, 0);

  /* phy_exynos_usb_v3p1_tune(): host-mode values */
  V = Rd (USBPHY_BASE, PHY_HSP_TUNE);
  V &= ~((0xFU << 28) | (0x3 << 24) | (0x3 << 21) | (0x3 << 18) | (0x7 << 8) | 0x7);
  V |= ((UINT32)TUNE_TXVREF << 28) | (TUNE_TXRISE << 24) | (TUNE_TXRES << 21) |
       (TUNE_TXPREEMP << 18) | (TUNE_SQRX << 8) | TUNE_COMPDIS;
  Wr (USBPHY_BASE, PHY_HSP_TUNE, V);

  /* phy_power_en(1): clear SIDDQ */
  Upd (USBPHY_BASE, PHY_HSP_TEST, 0, HSP_TEST_SIDDQ);
  MicroSecondDelay (10);

  /* phy_sw_rst_low */
  Upd (USBPHY_BASE, PHY_CLKRST, CLKRST_PHY20_RST_SEL, 0);
  Upd (USBPHY_BASE, PHY_CLKRST, 0,
       CLKRST_PHY20_SW_RST | CLKRST_PHY30_SW_RST | CLKRST_PORT_RST);
  MicroSecondDelay (75);

  /* use_io_for_ovc: route over-current to the pad, not the hub */
  Upd (USBPHY_BASE, PHY_LINK_PORT, 0, LINKPORT_OCD_U3 | LINKPORT_OCD_U2);

  /* phy_exynos_usb_v3p1_config_host_mode() */
  Upd (USBPHY_BASE, PHY_UTMI, UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN,
       UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
  Upd (USBPHY_BASE, PHY_HSP, 0, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT);

  Say ("PHY UTMI=0x%08x HSP=0x%08x TUNE=0x%08x TEST=0x%08x CLKRST=0x%08x",
       Rd (USBPHY_BASE, PHY_UTMI), Rd (USBPHY_BASE, PHY_HSP),
       Rd (USBPHY_BASE, PHY_HSP_TUNE), Rd (USBPHY_BASE, PHY_HSP_TEST),
       Rd (USBPHY_BASE, PHY_CLKRST));
}

STATIC BOOLEAN Dwc3HostMode (VOID)
{
  UINT32 V, Id;
  UINTN  T;

  Id = Rd (USBCTRL_BASE, DWC3_GSNPSID);
  Say ("DWC3 GSNPSID = 0x%08x (expect 0x5533xxxx)", Id);
  if ((Id & 0xFFFF0000) != 0x55330000 && (Id & 0xFFFF0000) != 0x33310000) {
    Say ("not a DWC3 ID: link clock/power domain is probably off");
    return FALSE;
  }

  /* dwc3_core_soft_reset() */
  Upd (USBCTRL_BASE, DWC3_DCTL, DCTL_CSFTRST, 0);
  for (T = 0; T < 5000; T++) {
    if (!(Rd (USBCTRL_BASE, DWC3_DCTL) & DCTL_CSFTRST)) {
      break;
    }
    MicroSecondDelay (100);
  }
  if (T == 5000) {
    Say ("DWC3 soft reset did not complete");
    return FALSE;
  }
  Upd (USBCTRL_BASE, DWC3_GUSB2PHYCFG, 0, GUSB2PHYCFG_PHYSOFTRST | GUSB2PHYCFG_SUSPHY);
  Upd (USBCTRL_BASE, DWC3_GUSB3PIPECTL, 0, GUSB3PIPECTL_PHYSOFTRST | GUSB3PIPECTL_SUSPHY);
  MicroSecondDelay (1000);

  V = Rd (USBCTRL_BASE, DWC3_GCTL);
  V = (V & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_HOST;
  Wr (USBCTRL_BASE, DWC3_GCTL, V);
  MicroSecondDelay (1000);
  Say ("DWC3 GCTL = 0x%08x (PRTCAPDIR bits 13:12 should be 1 = host)", Rd (USBCTRL_BASE, DWC3_GCTL));
  /* For comparing with Linux dwc3_core_init(): PHY width / turnaround come from here. */
  Say ("DWC3 GHWPARAMS3 = 0x%08x GUSB2PHYCFG = 0x%08x GUSB3PIPECTL = 0x%08x",
       Rd (USBCTRL_BASE, 0xC14C), Rd (USBCTRL_BASE, DWC3_GUSB2PHYCFG),
       Rd (USBCTRL_BASE, DWC3_GUSB3PIPECTL));
  return TRUE;
}

STATIC VOID XhciDump (VOID)
{
  UINT32 Cap = Rd (USBCTRL_BASE, 0x00);
  UINT32 Len = Cap & 0xFF;
  UINT32 Hcs1 = Rd (USBCTRL_BASE, 0x04);
  UINT32 Ports = (Hcs1 >> 24) & 0xFF;
  UINT32 P;

  Say ("xHCI CAPLENGTH=0x%02x HCIVERSION=0x%04x HCSPARAMS1=0x%08x ports=%u",
       Len, Cap >> 16, Hcs1, Ports);
  Say ("xHCI USBCMD=0x%08x USBSTS=0x%08x", Rd (USBCTRL_BASE, Len), Rd (USBCTRL_BASE, Len + 4));
  for (P = 0; P < Ports && P < 4; P++) {
    Say ("xHCI PORTSC%u = 0x%08x", P + 1, Rd (USBCTRL_BASE, Len + 0x400 + P * 0x10));
  }
}

/* Show the log on the console right before the boot menu, for 15 s, so it can be photographed. */
STATIC VOID EFIAPI ShowLog (IN EFI_EVENT Event, IN VOID *Context)
{
  gBS->CloseEvent (Event);
  if (gST->ConOut != NULL) {
    AsciiPrint ("---- ExynosUsbHostDxe stage 1 log ----\n%a", mLog);
    gBS->Stall (15 * 1000 * 1000);
  }
}

EFI_STATUS EFIAPI ExynosUsbHostEntry (IN EFI_HANDLE Image, IN EFI_SYSTEM_TABLE *St)
{
  EFI_EVENT Ev;

  Say ("stage 1 start");

  Upd (PMU_BASE, 0x704, PMU_PHY_ENABLE, 0);
  Say ("PMU USBDRD_PHY_CONTROL = 0x%08x", MmioRead32 (PMU_USBDRD_PHY));

  PhyEnableHost ();
  if (Dwc3HostMode ()) {
    XhciDump ();
  }

#if EXYNOS_USB_VBUS
  Say ("VBUS %a", VbusOn () ? "ON" : "FAILED");
  MicroSecondDelay (300000);
  XhciDump ();    /* a plugged device shows CCS (bit 0) in PORTSC after VBUS is up */
#endif

  {
    EFI_HANDLE  H = NULL;
    EFI_STATUS  R;

    R = RegisterNonDiscoverableMmioDevice (
          NonDiscoverableDeviceTypeXhci,
          NonDiscoverableDeviceDmaTypeNonCoherent,
          NULL, &H, 1, (UINTN)USBCTRL_BASE, (UINTN)0x10000);
    Say ("register xHCI with EDK2 USB stack: %r", R);
  }

  Say ("stage 1 done (log is %u bytes)", (UINT32)mLogLen);
  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ShowLog, NULL,
                      &gEfiEventReadyToBootGuid, &Ev);
  return EFI_SUCCESS;
}
