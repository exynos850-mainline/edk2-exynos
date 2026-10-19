#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BitmapLib.h>
#include <Library/KeypadDeviceImplLib.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>
#include <Protocol/KeypadDevice.h>
#include <Library/DebugLib.h>

// Set the key pins to GPIO input before reading them (the bootloader may have
// left them in EINT mode, where the DAT register is not reliable).
#define KEYPAD_FORCE_INPUT  1

// On-screen KEYPAD: log lines (they overlap the GUI, so off by default).
#define KEYPAD_DEBUG        0

// Holding Volume Down this long sends Enter (select).  Volume Down no longer
// auto-repeats; Volume Up still does.
#define KEYPAD_SELECT_HOLD_MS  700

typedef struct {
  KEY_CONTEXT EfiKeyContext;
  UINT32 PioBase;
  UINT32 BankOffset;
  UINT32 PinNum;
} KEY_CONTEXT_PRIVATE;


UINTN gBitmapScanCodes[BITMAP_NUM_WORDS(0x18)]    = {0};
UINTN gBitmapUnicodeChars[BITMAP_NUM_WORDS(0x7f)] = {0};


EFI_KEY_DATA gKeyDataPowerDown = {.Key = {
                                      .UnicodeChar = (CHAR16)'s',
                                  }};

EFI_KEY_DATA gKeyDataPowerUp = {.Key = {
                                    .UnicodeChar = (CHAR16)'e',
                                }};

EFI_KEY_DATA gKeyDataPowerLongpress = {.Key = {
                                           .UnicodeChar = (CHAR16)' ',
                                       }};

EFI_KEY_DATA gKeyDataEnter = {.Key = {
                                  .UnicodeChar = CHAR_CARRIAGE_RETURN,
                              }};


#define MS2NS(ms) (((UINT64)(ms)) * 1000000ULL)

STATIC inline VOID
KeySetState(UINT16 ScanCode, CHAR16 UnicodeChar, BOOLEAN Value)
{
  if (ScanCode && ScanCode < 0x18) {
    if (Value)
      BitmapSet(gBitmapScanCodes, ScanCode);
    else
      BitmapClear(gBitmapScanCodes, ScanCode);
  }

  if (UnicodeChar && UnicodeChar < 0x7f) {
    if (Value)
      BitmapSet(gBitmapUnicodeChars, ScanCode);
    else
      BitmapClear(gBitmapUnicodeChars, ScanCode);
  }
}

STATIC inline BOOLEAN KeyGetState(UINT16 ScanCode, CHAR16 UnicodeChar)
{
  if (ScanCode && ScanCode < 0x18) {
    if (!BitmapTest(gBitmapScanCodes, ScanCode))
      return FALSE;
  }

  if (UnicodeChar && UnicodeChar < 0x7f) {
    if (!BitmapTest(gBitmapUnicodeChars, ScanCode))
      return FALSE;
  }

  return TRUE;
}

STATIC inline VOID LibKeyInitializeKeyContext(KEY_CONTEXT *Context)
{
  SetMem(&Context->KeyData, sizeof(Context->KeyData), 0);
  Context->Time      = 0;
  Context->State     = KEYSTATE_RELEASED;
  Context->Repeat    = FALSE;
  Context->Longpress = FALSE;
}

STATIC inline VOID LibKeyUpdateKeyStatus(
    KEY_CONTEXT *Context, KEYPAD_RETURN_API *KeypadReturnApi, BOOLEAN IsPressed,
    UINT64 Delta)
{
  // keep track of the actual state
  KeySetState(
      Context->KeyData.Key.ScanCode, Context->KeyData.Key.UnicodeChar,
      IsPressed);

  // update key time
  Context->Time += Delta;

  switch (Context->State) {
  case KEYSTATE_RELEASED:
    if (IsPressed) {
      // change to pressed
      Context->Time  = 0;
      Context->State = KEYSTATE_PRESSED;
    }
    break;

  case KEYSTATE_PRESSED:
    if (IsPressed) {
      // keyrepeat
      if (Context->Repeat && Context->Time >= MS2NS(200)) {
        KeypadReturnApi->PushEfikeyBufTail(KeypadReturnApi, &Context->KeyData);
        Context->Time   = 0;
        Context->Repeat = TRUE;
      }

      else if (
          !Context->Longpress &&
          Context->Time >= MS2NS(
              (Context->KeyData.Key.ScanCode == SCAN_DOWN)
                  ? KEYPAD_SELECT_HOLD_MS : 500)) {
        // POWER, handle key combos
        if (Context->KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          if (KeyGetState(SCAN_DOWN, 0)) {
            // report 's'
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &gKeyDataPowerDown);
          }
          else if (KeyGetState(SCAN_UP, 0)) {
            // report 'e'
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &gKeyDataPowerUp);
          }
          else {
            // report spacebar
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &gKeyDataPowerLongpress);
          }
        }

        // post first keyrepeat event
        else {
          // only start keyrepeat if we're not doing a combo
          if (!KeyGetState(0, CHAR_CARRIAGE_RETURN)) {
            if (Context->KeyData.Key.ScanCode == SCAN_DOWN) {
              // long press on Volume Down = select
              KeypadReturnApi->PushEfikeyBufTail(
                  KeypadReturnApi, &gKeyDataEnter);
              Context->Time   = 0;
              Context->Repeat = FALSE;
            }
            else {
              KeypadReturnApi->PushEfikeyBufTail(
                  KeypadReturnApi, &Context->KeyData);
              Context->Time   = 0;
              Context->Repeat = TRUE;
            }
          }
        }

        Context->Longpress = TRUE;
      }
    }

    else {
      if (!Context->Longpress) {
        // we supressed down, so report it now
        KeypadReturnApi->PushEfikeyBufTail(KeypadReturnApi, &Context->KeyData);
        Context->State = KEYSTATE_LONGPRESS_RELEASE;
      }

      else if (Context->Time >= MS2NS(10)) {
        // we reported another key already
        Context->Time      = 0;
        Context->Repeat    = FALSE;
        Context->Longpress = FALSE;
        Context->State     = KEYSTATE_RELEASED;
      }
    }
    break;

  case KEYSTATE_LONGPRESS_RELEASE:
    // change to released
    Context->Time      = 0;
    Context->Repeat    = FALSE;
    Context->Longpress = FALSE;
    Context->State     = KEYSTATE_RELEASED;
    break;

  default:
    ASSERT(FALSE);
    break;
  }
}


STATIC KEY_CONTEXT_PRIVATE KeyContextPower;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeUp;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeDown;

STATIC KEY_CONTEXT_PRIVATE *KeyList[] = { &KeyContextVolumeDown, &KeyContextVolumeUp, &KeyContextPower };

STATIC
VOID KeypadInitializeKeyContextPrivate(KEY_CONTEXT_PRIVATE *Context)
{
  Context->PioBase    = 0;
  Context->BankOffset = 0;
  Context->PinNum     = 0;
}

STATIC
KEY_CONTEXT_PRIVATE *KeypadKeyCodeToKeyContext(UINT32 KeyCode)
{
  if (KeyCode == 114)
    return &KeyContextVolumeDown;
  else if (KeyCode == 115)
    return &KeyContextVolumeUp;
  else if (KeyCode == 116)
    return &KeyContextPower;
  else
    return NULL;
}

RETURN_STATUS
EFIAPI
KeypadDeviceImplConstructor(VOID)
{
  UINTN                Index;
  KEY_CONTEXT_PRIVATE *StaticContext;

  // Reset all keys
  for (Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {
    KeypadInitializeKeyContextPrivate(KeyList[Index]);
  }

  // Configure keys

  // vol down (gpa1-0)
  StaticContext             = KeypadKeyCodeToKeyContext(114);
  StaticContext->PioBase    = 0x11850000;
  StaticContext->BankOffset = 0x20;
  StaticContext->PinNum     = 0;

  // vol up (gpa0-7)
  StaticContext             = KeypadKeyCodeToKeyContext(115);
  StaticContext->PioBase    = 0x11850000;
  StaticContext->BankOffset = 0x00;
  StaticContext->PinNum     = 7;

  // ---- diagnostics: shown on screen at boot (remove once the keys work) ----
  for (Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {
    KEY_CONTEXT_PRIVATE *C = KeyList[Index];
    UINT32               Reg;

    if (C->PioBase == 0) {
      continue;
    }
    Reg = C->PioBase + C->BankOffset;

#if KEYPAD_FORCE_INPUT
    MmioAnd32(Reg + 0x0, ~(0xFu << (C->PinNum * 4)));
#endif

#if KEYPAD_DEBUG
    DEBUG((DEBUG_ERROR,
      "KEYPAD: key%d reg=%08x pin=%d CON=%08x DAT=%02x PUD=%04x\n",
      (int)Index, Reg, (int)C->PinNum,
      MmioRead32(Reg + 0x0), MmioRead32(Reg + 0x4) & 0xFF,
      MmioRead32(Reg + 0x8) & 0xFFFF));
#endif
  }

  // power: NOT configured yet.  On the A145F the power key is probably behind
  // the PMIC; take the pin from the stock DTB (gpio-keys node) and fill in:
  //   StaticContext             = KeypadKeyCodeToKeyContext(116);
  //   StaticContext->PioBase    = 0x11850000;   // bank base from the DTB
  //   StaticContext->BankOffset = 0x00;         // gpaN * 0x20
  //   StaticContext->PinNum     = 0;            // pin inside the bank

  return RETURN_SUCCESS;
}

EFI_STATUS EFIAPI KeypadDeviceImplReset(KEYPAD_DEVICE_PROTOCOL *This)
{
  LibKeyInitializeKeyContext(&KeyContextVolumeDown.EfiKeyContext);
  KeyContextVolumeDown.EfiKeyContext.KeyData.Key.ScanCode = SCAN_DOWN;

  LibKeyInitializeKeyContext(&KeyContextVolumeUp.EfiKeyContext);
  KeyContextVolumeUp.EfiKeyContext.KeyData.Key.ScanCode = SCAN_UP;

  LibKeyInitializeKeyContext(&KeyContextPower.EfiKeyContext);
  KeyContextPower.EfiKeyContext.KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;

  return EFI_SUCCESS;
}

EFI_STATUS KeypadDeviceImplGetKeys(
    KEYPAD_DEVICE_PROTOCOL *This, KEYPAD_RETURN_API *KeypadReturnApi,
    UINT64 Delta)
{
    BOOLEAN IsPressed;
    UINTN Index;
    UINT32 Raw = 0;
#if KEYPAD_DEBUG
    STATIC UINT32  LastRaw = 0xFFFFFFFF;
    STATIC BOOLEAN PollAnnounced = FALSE;
#endif
    STATIC UINT64  ComboTime = 0;
    STATIC BOOLEAN ComboFired = FALSE;

#if KEYPAD_DEBUG
    if (!PollAnnounced) {
        PollAnnounced = TRUE;
        DEBUG((DEBUG_ERROR, "KEYPAD: poll running\n"));
    }
#endif

    for (Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {
        KEY_CONTEXT_PRIVATE *Context = KeyList[Index];

        // Skip keys that have no GPIO pin configured.  The power key of the
        // A145F is not wired to a GPIO we know of (PioBase == 0), and reading
        // MMIO address 0x4 hangs or aborts the whole firmware.
        if (Context->PioBase == 0) {
            continue;
        }

        IsPressed = FALSE;

  		UINT32 PinAddr = ((Context->PioBase + Context->BankOffset) + 0x4);

        UINT32 PinState = MmioRead32(PinAddr);
        Raw |= ((PinState >> Context->PinNum) & 1) << Index;

        if ( !(PinState & (1 << Context->PinNum)) ) {
        	IsPressed = TRUE;
        }

        LibKeyUpdateKeyStatus(
            &Context->EfiKeyContext, KeypadReturnApi, IsPressed, Delta);
    }

#if KEYPAD_DEBUG
    if (Raw != LastRaw) {
        LastRaw = Raw;
        // bit0 = vol down, bit1 = vol up (1 = released, 0 = pressed)
        DEBUG((DEBUG_ERROR, "KEYPAD: raw=%x (bit0 voldown, bit1 volup)\n", Raw));
    }
#endif

    // No power key yet: Volume Up + Volume Down held together = Enter (select).
    // bit0 = vol down, bit1 = vol up in Raw; 0 means pressed.
    if ((Raw & 3) == 0) {
        ComboTime += Delta;
        if (!ComboFired && ComboTime >= MS2NS(80)) {
            ComboFired = TRUE;
            // swallow the normal Up/Down events these two releases would send
            KeyContextVolumeDown.EfiKeyContext.Longpress = TRUE;
            KeyContextVolumeDown.EfiKeyContext.Repeat    = FALSE;
            KeyContextVolumeUp.EfiKeyContext.Longpress   = TRUE;
            KeyContextVolumeUp.EfiKeyContext.Repeat      = FALSE;
            KeypadReturnApi->PushEfikeyBufTail(KeypadReturnApi, &gKeyDataEnter);
        }
    }
    else if ((Raw & 3) == 3) {
        ComboTime  = 0;
        ComboFired = FALSE;
    }

    return EFI_SUCCESS;
}

