/*
 * hwinfo.c - dashboard sensors for the setup screen.
 *
 * What is real, and what is not (stated honestly per the design brief):
 *
 *   Time, date, weekday   - EFI runtime services GetTime.                REAL
 *   CPU brand string      - CPUID leaves 0x80000002..0x80000004.         REAL
 *   CPU frequency/volt    - AMD P-state MSRs: MSR_PSTATE_STATUS
 *                           (0xC0010296) gives the current P-state,
 *                           MSR_PSTATE0..7 (0xC0010064+n) carry StC
 *                           (core frequency in 100 MHz steps) and VfSel
 *                           (voltage in 5 mV steps), the same decoding the
 *                           Linux amd-pstate legacy driver uses.  Read
 *                           only on AMD family 15h model 60h..9Fh (the
 *                           box's A9-9420 reports 15h/60h); RDMSR of an
 *                           unsupported MSR raises #GP, so the gate is
 *                           strict.                                       REAL
 *   CPU temperature       - SMU v8 path (this box): the northbridge
 *                           indirect pair of PCI dev 0 fn 0 (index 0xB8,
 *                           data 0xBC) reading SMN 0xD8200CA4, decoded as
 *                           (val >> 21) with the -49 range bit 19, exactly
 *                           like Linux k10temp for family 15h models
 *                           60h-7Fh.  SMU v9+ path: PCI dev 18 fn 3
 *                           (index 0x154, data 0x15C, register 0x99040)
 *                           with plausible-field probing.  Implausible
 *                           readings keep the mock.                    BEST EFFORT
 *   BCLK 100 MHz          - the Stoney clock-generator strap is fixed at
 *                           100 MHz by hardware; nothing readable.      FIXED
 *   Memory freq/volt,     - trained by the PSP/SMU firmware and only
 *   iGPU freq/volt          reachable through SMU messages that must not
 *                           be issued from a UEFI app (they can hang the
 *                           SMU).  Values are the mockup's, shown as
 *                           placeholders until the real firmware module
 *                           can ask the SMU properly.                    STUB
 */

#include "SbBios.h"

#define MSR_PSTATE_STATUS   0xC0010296u
#define MSR_PSTATE0         0xC0010064u

/* SMU v8 (family 15h models 60h-7Fh): northbridge indirect window */
#define NB_INDEX_REG        0xB8
#define NB_DATA_REG         0xBC
#define NB_TEMP_OFFSET      0xD8200CA4u
#define NB_TEMP_SHIFT       21
#define NB_TEMP_RANGE_BIT   0x00080000u    /* -49 degC range select  */

/* SMU9 indirect access (config space of PCI dev 18, function 3) */
#define SMU9_INDEX_REG      0x154
#define SMU9_DATA_REG       0x15C
#define SMU9_TEMP_INPUT     0x99040u

STATIC BOOLEAN  mProbed = FALSE;
STATIC BOOLEAN  mCpuReal = FALSE;
STATIC BOOLEAN  mTempReal = FALSE;
STATIC BOOLEAN  mSmu8 = FALSE;         /* family 15h model 60h..9Fh */
STATIC UINTN    mCpuFreqMHz = 0;
STATIC UINTN    mCpuVoltMv = 0;
STATIC UINTN    mTempC = 0;
STATIC CHAR16   mBrand[64];

STATIC
VOID
ProbeCpu (
  VOID
  )
{
  UINT32  Eax, Ebx, Ecx, Edx;
  UINT32  Family, Model;
  CHAR8   Raw[49];
  UINTN   I, Out;

  /* brand string */
  Raw[0] = 0;
  for (I = 0; I < 3; I++) {
    AsmCpuid (0x80000002u + (UINT32)I, &Eax, &Ebx, &Ecx, &Edx);
    CopyMem (&Raw[I * 16], &Eax, 4);
    CopyMem (&Raw[I * 16 + 4], &Ebx, 4);
    CopyMem (&Raw[I * 16 + 8], &Ecx, 4);
    CopyMem (&Raw[I * 16 + 12], &Edx, 4);
  }
  Raw[48] = 0;
  Out = 0;
  for (I = 0; I < 48 && Out < 62; I++) {
    if (Raw[I] >= ' ' && Raw[I] < 0x7F) {
      mBrand[Out++] = (CHAR16)Raw[I];
    }
  }
  mBrand[Out] = 0;
  /* trim the trailing padding the brand strings are padded with */
  while (Out > 0 && mBrand[Out - 1] == L' ') {
    mBrand[--Out] = 0;
  }

  /* family/model gate: RDMSR of AMD P-state MSRs only where defined */
  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  Family = ((Eax >> 8) & 0xF) + ((Eax >> 20) & 0xFF);
  Model  = ((Eax >> 4) & 0xF) | ((Eax >> 16) & 0xF0);
  SbLog ("[hw] fam model", Family, Model);
  if (Family != 0x15 || Model < 0x60 || Model > 0x9F) {
    return;                       /* not Carrizo/Stoney: keep the mock */
  }
  mSmu8 = TRUE;                   /* SMU v8 register map for temp */

  {
    UINT64  Status = AsmReadMsr64 (MSR_PSTATE_STATUS);
    UINTN   PState = (UINTN)(Status & 0x7);
    UINT64  Ps     = AsmReadMsr64 (MSR_PSTATE0 + PState);
    UINTN   StC    = (UINTN)(Ps & 0xFF);          /* 100 MHz steps     */
    UINTN   VfSel  = (UINTN)((Ps >> 8) & 0xFF);   /* 5 mV steps        */

    mCpuFreqMHz = StC * 100;
    mCpuVoltMv  = VfSel * 5;
    mCpuReal    = (StC != 0);
    SbLog ("[hw] pstate", PState, mCpuFreqMHz);
    SbLog ("[hw] vid", VfSel, mCpuVoltMv);
  }
}

/*
 * AMD northbridge/SMU indirect read: write the SMN address to an index
 * register, read the data register back.  Used for the SMU v8
 * (dev 0 fn 0, 0xB8/0xBC) and SMU v9 (dev 18 fn 3, 0x154/0x15C) maps.
 */
STATIC
EFI_STATUS
NbIndirectRead (
  IN  EFI_PCI_IO_PROTOCOL  *Pci,
  IN  UINTN                IndexReg,
  IN  UINTN                DataReg,
  IN  UINT32               SmnAddress,
  OUT UINT32               *Value
  )
{
  EFI_STATUS  Status;

  *Value = 0;
  Status = Pci->Pci.Write (Pci, EfiPciIoWidthUint32, IndexReg, 1,
                           &SmnAddress);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Pci->Pci.Read (Pci, EfiPciIoWidthUint32, DataReg, 1, Value);
}

/*
 * Temperature.  The box reports CPUID family 15h model 60h (SMU v8):
 * read the Reported Temperature register through the northbridge
 * indirect window of dev 0 fn 0, decoding (val >> 21) with the -49
 * range bit exactly like Linux k10temp's F15H_M60H path.  On newer
 * silicon fall back to the SMU v9 pair on dev 18 fn 3 with
 * plausible-field probing.  Anything implausible keeps the mock.
 */
STATIC
VOID
ProbeTemp (
  VOID
  )
{
  EFI_HANDLE       *Handles;
  EFI_STATUS       Status;
  UINTN            Index, Count;
  EFI_PCI_IO_PROTOCOL  *Pci;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid,
                                     NULL, &Count, &Handles);
  SbLog ("[hw] pci handles", Count, (UINTN)Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    UINTN   Seg, Bus, Dev, Func;
    UINT32  Id, Reg = 0;

    Pci = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid,
                                        (VOID **)&Pci)) || (Pci == NULL)) {
      continue;
    }
    if (EFI_ERROR (Pci->GetLocation (Pci, &Seg, &Bus, &Dev, &Func))) {
      continue;
    }
    if ((Seg != 0) || (Bus != 0)) {
      continue;
    }
    if (mSmu8) {
      if ((Dev != 0) || (Func != 0)) {
        continue;
      }
    } else if ((Dev != 18) || (Func != 3)) {
      continue;
    }
    Status = Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id);
    if (EFI_ERROR (Status) || ((Id & 0xFFFF) != 0x1022)) {
      continue;
    }

    if (mSmu8) {
      Status = NbIndirectRead (Pci, NB_INDEX_REG, NB_DATA_REG,
                               NB_TEMP_OFFSET, &Reg);
      SbLog6 ("[hw] nb temp raw", (UINTN)Status, Reg, Reg >> NB_TEMP_SHIFT,
              0, 0, 0);
      if (!EFI_ERROR (Status)) {
        INT32  Temp = (INT32)(Reg >> NB_TEMP_SHIFT);

        if ((Reg & NB_TEMP_RANGE_BIT) != 0) {
          Temp -= 49;
        }
        if ((Temp >= 15) && (Temp <= 120)) {
          mTempC    = (UINTN)Temp;
          mTempReal = TRUE;
        }
      }
    } else {
      Status = NbIndirectRead (Pci, SMU9_INDEX_REG, SMU9_DATA_REG,
                               SMU9_TEMP_INPUT, &Reg);
      SbLog ("[hw] smu temp raw", Status, Reg);
      if (!EFI_ERROR (Status)) {
        /* undocumented packing: probe the plausible byte fields */
        UINTN  Cand[3] = { (Reg >> 16) & 0xFF, (Reg >> 8) & 0xFF, Reg & 0xFF };
        UINTN  I;

        for (I = 0; I < 3; I++) {
          if (Cand[I] >= 15 && Cand[I] <= 120) {
            mTempC    = Cand[I];
            mTempReal = TRUE;
            break;
          }
        }
      }
    }
    if (mTempReal) {
      break;
    }
  }

  FreePool (Handles);
}

/* "1234" MHz -> "1.2 GHz" (one decimal, truncated) */
STATIC
VOID
FmtGHz (
  OUT CHAR16  *Out,
  IN  UINTN   OutChars,
  IN  UINTN   Mhz
  )
{
  UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%u.%u GHz",
                 Mhz / 1000, (Mhz / 100) % 10);
}

/* millivolts -> "1.25V" */
STATIC
VOID
FmtVolt (
  OUT CHAR16  *Out,
  IN  UINTN   OutChars,
  IN  UINTN   Mv
  )
{
  UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%u.%02uV",
                 Mv / 1000, (Mv % 1000) / 10);
}

STATIC
VOID
ClockToText (
  OUT CHAR16  *Time,
  OUT CHAR16  *Date,
  OUT CHAR16  *Day
  )
{
  EFI_TIME  T;
  STATIC CONST CHAR16  *Days[] = {
    L"Monday", L"Tuesday", L"Wednesday", L"Thursday",
    L"Friday", L"Saturday", L"Sunday"
  };
  UINTN  Y = 2000, M = 1, D = 1, W;

  if (!EFI_ERROR (gRT->GetTime (&T, NULL))) {
    Y = T.Year; M = T.Month; D = T.Day;
    UnicodeSPrint (Time, 16 * sizeof (CHAR16), L"%02u:%02u",
                   (UINTN)T.Hour, (UINTN)T.Minute);
  } else {
    UnicodeSPrint (Time, 16 * sizeof (CHAR16), L"--:--");
  }
  UnicodeSPrint (Date, 16 * sizeof (CHAR16), L"%02u/%02u/%04u", M, D, Y);

  /* Sakamoto's algorithm: weekday index with the year shifted in winter */
  {
    STATIC CONST UINTN  Tbl[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    UINTN  Yy = Y;
    if (M < 3) {
      Yy -= 1;
    }
    W = (Yy + Yy / 4 - Yy / 100 + Yy / 400 + Tbl[M - 1] + D) % 7;
  }
  StrCpyS (Day, 16, Days[W]);
}

VOID
HwUpdate (
  OUT HW_INFO  *Hw
  )
{
  if (!mProbed) {
    ProbeCpu ();
    ProbeTemp ();
    mProbed = TRUE;
    SbLog ("[hw] probe done", mCpuReal, mTempReal);
  }

  ZeroMem (Hw, sizeof (*Hw));
  ClockToText (Hw->Time, Hw->Date, Hw->Weekday);
  StrCpyS (Hw->Brand, 64, mBrand);
  Hw->CpuReal  = mCpuReal;
  Hw->TempReal = mTempReal;

  if (mCpuReal) {
    FmtGHz (Hw->CpuFreq, 16, mCpuFreqMHz);
    FmtVolt (Hw->CpuVolt, 16, mCpuVoltMv);
  } else {
    StrCpyS (Hw->CpuFreq, 16, L"3.6 GHz");   /* mock: CPU not probeable */
    StrCpyS (Hw->CpuVolt, 16, L"1.25V");
  }
  if (mTempReal) {
    UnicodeSPrint (Hw->CpuTemp, 16 * sizeof (CHAR16), L"%uC", mTempC);
  } else {
    StrCpyS (Hw->CpuTemp, 16, L"44C");       /* mock: SMU read rejected  */
  }

  /*
   * Fixed / stub values, see the file header: BCLK is the hardware strap,
   * memory and iGPU numbers are the design mockups.
   */
  StrCpyS (Hw->CpuBclk, 16, L"100 MHz");
  StrCpyS (Hw->MemFreq, 16, L"2133 MHz");
  StrCpyS (Hw->MemVolt, 16, L"1.2V");
  StrCpyS (Hw->GpuFreq, 16, L"960 MHz");
  StrCpyS (Hw->GpuVolt, 16, L"1.0V");
}
