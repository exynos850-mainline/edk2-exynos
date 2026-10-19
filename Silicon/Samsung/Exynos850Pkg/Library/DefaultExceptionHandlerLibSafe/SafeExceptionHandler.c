/** @file
  Robust AArch64 exception handler for Exynos850 (SM-A145F).

  The upstream handler walks the frame-pointer chain without validating it, so
  a corrupt chain makes the handler fault ("Recursive exception occurred while
  dumping the CPU state") before ESR/FAR/registers are ever printed. This
  version prints the vital state first, validates every pointer against the
  platform memory map, and runs each risky stage under SetJump/LongJump so that
  a fault inside the handler is reported and the dump continues.

  All output goes through SerialPortWrite() only (framebuffer console on this
  platform), never through DEBUG()/ConOut, so it also works in RELEASE builds.

  Based on ArmPkg DefaultExceptionHandler.c
  Copyright (c) 2008 - 2010, Apple Inc. All rights reserved.<BR>
  Copyright (c) 2011 - 2014, ARM Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiLib.h>

#include <Guid/DebugImageInfoTable.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/LoadedImage.h>

#define EXC_MAX_FRAMES         24
#define EXC_STACK_BEFORE       64
#define EXC_STACK_AFTER        192
#define EXC_MAX_FRAME_STRIDE   0x100000ULL   // a frame record must be within 1 MiB of the previous one

STATIC CONST CHAR8 *CONST  gExceptionTypeString[] = {
  "Synchronous",
  "IRQ",
  "FIQ",
  "SError"
};

STATIC BASE_LIBRARY_JUMP_BUFFER  mJumpBuffer;
STATIC volatile UINTN            mDepth;   // 0 = not in handler
STATIC volatile BOOLEAN          mArmed;   // a SetJump point is live
STATIC volatile UINTN            mStage;

/** Print through the framebuffer console only; never allocates. **/
STATIC
VOID
ExcPrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[200];
  VA_LIST  Marker;
  UINTN    Len;

  VA_START (Marker, Format);
  Len = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  SerialPortWrite ((UINT8 *)Buffer, Len);
}

/**
  TRUE if [Address, Address+Size) lies inside system RAM that UEFI has mapped,
  according to the platform memory map. Anything else is not dereferenced.
**/
STATIC
BOOLEAN
IsMappedRam (
  IN UINT64  Address,
  IN UINTN   Size
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX  *Map;
  UINT64                           End;

  End = Address + Size;
  if (End < Address) {
    return FALSE;
  }

  for (Map = GetPlatformMemoryMap (); (Map != NULL) && (Map->Length != 0); Map++) {
    if ((Map->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (Address >= Map->Address) &&
        (End <= Map->Address + Map->Length))
    {
      return TRUE;
    }
  }

  return FALSE;
}

/** Find the loaded image containing Address. Returns the PDB path or NULL. **/
STATIC
CONST CHAR8 *
FindImage (
  IN  UINT64  Address,
  OUT UINT64  *ImageBase
  )
{
  EFI_STATUS                         Status;
  EFI_DEBUG_IMAGE_INFO_TABLE_HEADER  *Header;
  EFI_DEBUG_IMAGE_INFO               *Table;
  UINTN                              Entry;
  EFI_LOADED_IMAGE_PROTOCOL          *Li;

  Status = EfiGetSystemConfigurationTable (&gEfiDebugImageInfoTableGuid, (VOID **)&Header);
  if (EFI_ERROR (Status) || (Header == NULL) || (Header->EfiDebugImageInfoTable == NULL)) {
    return NULL;
  }

  Table = Header->EfiDebugImageInfoTable;
  for (Entry = 0; Entry < Header->TableSize; Entry++, Table++) {
    if ((Table->NormalImage == NULL) ||
        (Table->NormalImage->ImageInfoType != EFI_DEBUG_IMAGE_INFO_TYPE_NORMAL))
    {
      continue;
    }

    Li = Table->NormalImage->LoadedImageProtocolInstance;
    if (Li == NULL) {
      continue;
    }

    if ((Address >= (UINT64)(UINTN)Li->ImageBase) &&
        (Address < (UINT64)(UINTN)Li->ImageBase + Li->ImageSize))
    {
      *ImageBase = (UINT64)(UINTN)Li->ImageBase;
      return PeCoffLoaderGetPdbPointer (Li->ImageBase);
    }
  }

  return NULL;
}

STATIC
CONST CHAR8 *
BaseName (
  IN CONST CHAR8  *FullName
  )
{
  CONST CHAR8  *Str;

  Str = FullName + AsciiStrLen (FullName);
  while (--Str > FullName) {
    if ((*Str == '/') || (*Str == '\\')) {
      return Str + 1;
    }
  }

  return Str;
}

/** "label 0xADDR (Image.dll+0xOFFSET)" **/
STATIC
VOID
PrintLocation (
  IN CONST CHAR8  *Label,
  IN UINT64       Address
  )
{
  UINT64       Base;
  CONST CHAR8  *Pdb;

  Base = 0;
  Pdb  = FindImage (Address, &Base);
  if (Pdb != NULL) {
    ExcPrint ("%a 0x%012lx  %a+0x%lx\n", Label, Address, BaseName (Pdb), Address - Base);
  } else {
    ExcPrint ("%a 0x%012lx  (no image)\n", Label, Address);
  }
}

STATIC
VOID
DescribeInstructionOrDataAbort (
  IN CONST CHAR8  *AbortType,
  IN UINTN  Iss
  )
{
  CONST CHAR8  *AbortCause;

  switch (Iss & 0x3f) {
    case 0x0: AbortCause = "Address size fault, zeroth level of translation or translation table base register";
      break;
    case 0x1: AbortCause = "Address size fault, first level";
      break;
    case 0x2: AbortCause = "Address size fault, second level";
      break;
    case 0x3: AbortCause = "Address size fault, third level";
      break;
    case 0x4: AbortCause = "Translation fault, zeroth level";
      break;
    case 0x5: AbortCause = "Translation fault, first level";
      break;
    case 0x6: AbortCause = "Translation fault, second level";
      break;
    case 0x7: AbortCause = "Translation fault, third level";
      break;
    case 0x9: AbortCause = "Access flag fault, first level";
      break;
    case 0xa: AbortCause = "Access flag fault, second level";
      break;
    case 0xb: AbortCause = "Access flag fault, third level";
      break;
    case 0xd: AbortCause = "Permission fault, first level";
      break;
    case 0xe: AbortCause = "Permission fault, second level";
      break;
    case 0xf: AbortCause = "Permission fault, third level";
      break;
    case 0x10: AbortCause = "Synchronous external abort";
      break;
    case 0x18: AbortCause = "Synchronous parity error on memory access";
      break;
    case 0x11: AbortCause = "Asynchronous external abort";
      break;
    case 0x19: AbortCause = "Asynchronous parity error on memory access";
      break;
    case 0x14: AbortCause = "Synchronous external abort on translation table walk, zeroth level";
      break;
    case 0x15: AbortCause = "Synchronous external abort on translation table walk, first level";
      break;
    case 0x16: AbortCause = "Synchronous external abort on translation table walk, second level";
      break;
    case 0x17: AbortCause = "Synchronous external abort on translation table walk, third level";
      break;
    case 0x1c: AbortCause = "Synchronous parity error on memory access on translation table walk, zeroth level";
      break;
    case 0x1d: AbortCause = "Synchronous parity error on memory access on translation table walk, first level";
      break;
    case 0x1e: AbortCause = "Synchronous parity error on memory access on translation table walk, second level";
      break;
    case 0x1f: AbortCause = "Synchronous parity error on memory access on translation table walk, third level";
      break;
    case 0x21: AbortCause = "Alignment fault";
      break;
    case 0x22: AbortCause = "Debug event";
      break;
    case 0x30: AbortCause = "TLB conflict abort";
      break;
    case 0x33:
    case 0x34: AbortCause = "IMPLEMENTATION DEFINED";
      break;
    case 0x35:
    case 0x36: AbortCause = "Domain fault";
      break;
    default: AbortCause = "";
      break;
  }

  ExcPrint ("%a: %a\n", AbortType, AbortCause);
}

STATIC
VOID
DescribeExceptionSyndrome (
  IN UINT32  Esr
  )
{
  CONST CHAR8  *Message;
  UINTN  Ec;
  UINTN  Iss;

  Ec  = Esr >> 26;
  Iss = Esr & 0x00ffffff;

  switch (Ec) {
    case 0x15: Message = "SVC executed in AArch64";
      break;
    case 0x20:
    case 0x21: DescribeInstructionOrDataAbort ("Instruction abort", Iss);
      return;
    case 0x22: Message = "PC alignment fault";
      break;
    case 0x23: Message = "SP alignment fault";
      break;
    case 0x24:
    case 0x25: DescribeInstructionOrDataAbort ("Data abort", Iss);
      return;
    default: return;
  }

  ExcPrint (" %a\n", Message);
}


STATIC
VOID
StageRegisters (
  IN EFI_SYSTEM_CONTEXT_AARCH64  *C
  )
{
  ExcPrint (" X0 %016lx  X1 %016lx\n X2 %016lx  X3 %016lx\n", C->X0, C->X1, C->X2, C->X3);
  ExcPrint (" X4 %016lx  X5 %016lx\n X6 %016lx  X7 %016lx\n", C->X4, C->X5, C->X6, C->X7);
  ExcPrint (" X8 %016lx  X9 %016lx\nX10 %016lx X11 %016lx\n", C->X8, C->X9, C->X10, C->X11);
  ExcPrint ("X12 %016lx X13 %016lx\nX14 %016lx X15 %016lx\n", C->X12, C->X13, C->X14, C->X15);
  ExcPrint ("X16 %016lx X17 %016lx\nX18 %016lx X19 %016lx\n", C->X16, C->X17, C->X18, C->X19);
  ExcPrint ("X20 %016lx X21 %016lx\nX22 %016lx X23 %016lx\n", C->X20, C->X21, C->X22, C->X23);
  ExcPrint ("X24 %016lx X25 %016lx\nX26 %016lx X27 %016lx\n", C->X24, C->X25, C->X26, C->X27);
  ExcPrint ("X28 %016lx\n", C->X28);
}

STATIC
VOID
StageBacktrace (
  IN EFI_SYSTEM_CONTEXT_AARCH64  *C
  )
{
  UINT64  Fp;
  UINT64  Next;
  UINT64  Ret;
  UINTN   Depth;

  PrintLocation ("BT[ 0]", C->ELR);
  PrintLocation ("BT[ 1] (LR)", C->LR);

  Fp = C->FP;
  for (Depth = 2; Depth < EXC_MAX_FRAMES; Depth++) {
    if ((Fp == 0) || ((Fp & 0xF) != 0) || !IsMappedRam (Fp, 16)) {
      ExcPrint ("BT end: FP=0x%lx %a\n", Fp, (Fp == 0) ? "(null)" : "(invalid, stopped)");
      return;
    }

    Next = ((UINT64 *)Fp)[0];
    Ret  = ((UINT64 *)Fp)[1];
    if (Ret != C->LR) {
      ExcPrint ("BT[%2d]", Depth);
      PrintLocation ("", Ret);
    }

    if ((Next <= Fp) || (Next - Fp > EXC_MAX_FRAME_STRIDE)) {
      ExcPrint ("BT end: next FP=0x%lx not above 0x%lx\n", Next, Fp);
      return;
    }

    Fp = Next;
  }
}

STATIC
VOID
StageStack (
  IN EFI_SYSTEM_CONTEXT_AARCH64  *C
  )
{
  INT32   Offset;
  UINT64  Sp;

  Sp = C->SP;
  if ((Sp & 0x7) != 0) {
    ExcPrint ("SP 0x%lx unaligned, no stack dump\n", Sp);
    return;
  }

  ExcPrint ("Stack @ SP=0x%lx:\n", Sp);
  for (Offset = -EXC_STACK_BEFORE; Offset < EXC_STACK_AFTER; Offset += 32) {
    UINT64  A = Sp + Offset;
    if (!IsMappedRam (A, 32)) {
      continue;
    }

    ExcPrint ("%c%010lx %016lx %016lx\n %010lx %016lx %016lx\n",
      Offset == 0 ? '>' : ' ', A, ((UINT64 *)A)[0], ((UINT64 *)A)[1],
      A + 16, ((UINT64 *)A)[2], ((UINT64 *)A)[3]);
  }
}

#define RUN_STAGE(Id, Call)                                      \
  do {                                                           \
    mStage = (Id);                                               \
    if (SetJump (&mJumpBuffer) == 0) {                           \
      mArmed = TRUE;                                             \
      Call;                                                      \
      mArmed = FALSE;                                            \
    } else {                                                     \
      ExcPrint ("(stage %d aborted by fault above)\n", (Id));    \
    }                                                            \
  } while (0)

/**
  Default action on an unexpected exception. Must not allocate memory.
**/
VOID
DefaultExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *C;
  UINT32                      Esr;

  C = SystemContext.SystemContextAArch64;

  if (mDepth != 0) {
    //
    // A fault inside the handler. Report it with its own registers.
    //
    ExcPrint ("\n!! FAULT IN HANDLER stage %d\n ELR 0x%lx FAR 0x%lx ESR 0x%x\n",
      mStage, C->ELR, C->FAR, (UINT32)C->ESR);
    PrintLocation (" at", C->ELR);
    if (mArmed) {
      mArmed = FALSE;
      LongJump (&mJumpBuffer, 1);
    }

    ExcPrint ("Unrecoverable, halting.\n");
    CpuDeadLoop ();
  }

  mDepth = 1;
  Esr    = (UINT32)C->ESR;

  ExcPrint ("\n== %a EXCEPTION ==\n", ExceptionType <= EXCEPT_AARCH64_SERROR ? gExceptionTypeString[ExceptionType] : "Unknown");
  ExcPrint ("ELR 0x%016lx  ESR 0x%08x\nFAR 0x%016lx SPSR 0x%08x\n", C->ELR, Esr, C->FAR, (UINT32)C->SPSR);
  ExcPrint (" SP 0x%016lx   LR 0x%016lx\n FP 0x%016lx\n", C->SP, C->LR, C->FP);
  ExcPrint ("EC 0x%02x IL %d ISS 0x%07x", Esr >> 26, (Esr >> 25) & 1, Esr & 0x1FFFFFF);
  if (((Esr >> 26) == 0x24) || ((Esr >> 26) == 0x25)) {
    ExcPrint ("  %a%a", (Esr & BIT6) ? "WRITE" : "READ", (Esr & BIT10) ? ", FAR invalid" : "");
  }

  ExcPrint ("\n");

  RUN_STAGE (1, DescribeExceptionSyndrome (Esr));
  RUN_STAGE (2, PrintLocation ("PC", C->ELR));
  RUN_STAGE (3, StageBacktrace (C));
  RUN_STAGE (4, StageRegisters (C));
  RUN_STAGE (5, StageStack (C));

  ExcPrint ("== END OF EXCEPTION DUMP ==\n");
  CpuDeadLoop ();
}
