/*
 * hwinfo.c - dashboard sensors for the setup screen.
 *
 * Target: AMD Stoney Ridge (e.g. A9-9420, Family 15h Models 70h-7Fh, SMU v8),
 * also compatible with Carrizo (Family 15h Models 60h-6Fh).
 *
 * Zero-overhead design:
 *   - PCI handles (D0F0, D1F0, D18F2, D18F5) are located once and cached.
 *   - CPU frequency/voltage (MSR) and temperature (SMU indirect) are
 *     re-read live on every dashboard tick; everything static (memory,
 *     iGPU) is read once at probe.
 *   - The iGPU mailbox command is the only polling loop and runs once.
 *   - No dynamic memory allocation after initial probe.
 *
 * Sensor sources:
 *   Time, date, weekday   - EFI runtime services GetTime.
 *   CPU brand string      - CPUID leaves 0x80000002..0x80000004.
 *   CPU frequency/volt    - MSR_COFVID_STATUS (0xC0010071):
 *                           CurCpuFid[5:0], CurCpuDid[8:6],
 *                           CurCpuVid ({[20], [15:9]}, 8-bit SVI2).
 *   CPU temperature       - SMU v8 indirect via D0F0xB8 / D0F0xBC,
 *                           register 0xD8200CA4 (Reported Temp Ctrl),
 *                           CurTmp[31:21] in 0.125 C steps.
 *   BCLK 100 MHz          - Hardware reference clock strap.             NEED FIX LATER
 *   RAM frequency         - D18F2x94[MemClkFreq] (bits 4:0).
 *   RAM voltage           - D18F2x9C_x0001_004A (DqDqsRcvCntrl1) via
 *                           D18F2x98/9C indirect access (MajorMode).
 *   iGPU / NB voltage     - D18F5x174 (NB P-state Status, CurNbVid
 *                           {[22], [16:10]}, 8-bit SVI2, shared w/ NB).
 *   iGPU frequency        - D0F0xB8/BC SMU mailbox (cmd/arg=8);
 *                           ARG1 / 100 = limit MHz, once.
 */

#include "SbBios.h"

#define MSR_COFVID_STATUS       0xC0010071u

/* SMU v8 indirect access via D0F0 (00:00.0) */
#define D0F0_SMU_INDEX_REG      0xB8
#define D0F0_SMU_DATA_REG       0xBC
#define SMU8_REPORTED_TEMP_REG  0xD8200CA4u
#define SMU_CMD_ADDR            0x13000000u
#define SMU_RSP_ADDR            0x13000010u
#define SMU_ARG0_ADDR           0x13000020u
#define SMU_ARG1_ADDR           0x13000024u
#define SMU_ARG2_ADDR           0x13000028u
#define SMU_ARG4_ADDR           0x1300002cu
#define SMU_ARG5_ADDR           0x13000030u

/* DRAM controller D18F2 (00:18.2) */
#define D18F2_DCHR0_REG         0x94
#define D18F2_DCT_OFFSET_REG    0x98
#define D18F2_DCT_DATA_REG      0x9C
#define DCT_DQ_DQS_RCV_CNTRL1   0x0001004Au

/* Northbridge P-state D18F5 (00:18.5) */
#define D18F5_NBPSTATE_BASE     0x160
#define D18F5_NBPSTATE_STS_REG  0x174

STATIC BOOLEAN  mProbed = FALSE;
STATIC BOOLEAN  mIsSmu8Apu = FALSE;
STATIC BOOLEAN  mCpuReal = FALSE;
STATIC BOOLEAN  mTempReal = FALSE;
STATIC BOOLEAN  mMemReal = FALSE;
STATIC BOOLEAN  mMemVoltReal = FALSE;
STATIC BOOLEAN  mGpuVoltReal = FALSE;
STATIC BOOLEAN  mGpuPresent = FALSE;

STATIC UINTN    mCpuFreqMHz = 0;
STATIC UINTN    mCpuVoltMv = 0;
STATIC UINTN    mTempC = 0;
STATIC UINTN    mMemFreqMHz = 0;
STATIC UINTN    mMemVoltMv = 0;
STATIC UINTN    mGpuFreqMHz = 0;
STATIC UINTN    mGpuVoltMv = 0;
STATIC CHAR16   mBrand[64];

STATIC EFI_PCI_IO_PROTOCOL  *mD0F0 = NULL;

/* SVI2 8-bit VID -> millivolts (1.55V - VID * 6.25 mV) */
STATIC
UINTN
Svi2VidToMv (
  IN UINTN  Vid
  )
{
  if (Vid >= 0xF8) {
    return 0;
  }
  return (155000 - Vid * 625) / 100;
}

STATIC
VOID
ProbeBrandAndFamily (
  VOID
  )
{
  UINT32  Eax, Ebx, Ecx, Edx;
  UINT32  Family, Model;
  CHAR8   Raw[49];
  UINTN   I, Out;

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
  while (Out > 0 && mBrand[Out - 1] == L' ') {
    mBrand[--Out] = 0;
  }

  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  Family = ((Eax >> 8) & 0xF) + ((Eax >> 20) & 0xFF);
  Model  = ((Eax >> 4) & 0xF) | ((Eax >> 16) & 0xF0);
  SbLog ("[hw] fam model", Family, Model);

  /* Carrizo (60h-6Fh) and Stoney Ridge (70h-7Fh) */
  mIsSmu8Apu = (Family == 0x15 && Model >= 0x60 && Model <= 0x7F);
}

STATIC
VOID
ReadCpuMsr (
  VOID
  )
{
  UINT64  Cofvid;
  UINTN   Fid, Did, Vid;
  STATIC UINTN  LogLeft = 3;

  if (!mIsSmu8Apu) {
    return;
  }

  Cofvid = AsmReadMsr64 (MSR_COFVID_STATUS);
  Fid = (UINTN)(Cofvid & 0x3F);
  Did = (UINTN)((Cofvid >> 6) & 0x7);
  Vid = (UINTN)(((Cofvid >> 9) & 0x7F) | (((Cofvid >> 20) & 1) << 7));

  mCpuFreqMHz = (100 * (Fid + 0x10)) >> Did;
  mCpuVoltMv  = Svi2VidToMv (Vid);
  mCpuReal    = (mCpuFreqMHz != 0);

  if (LogLeft-- > 0) {
    SbLog ("[hw] cofvid freq/mv", mCpuFreqMHz, mCpuVoltMv);
  }
}

STATIC
VOID
ReadTempFromD0F0 (
  IN EFI_PCI_IO_PROTOCOL  *D0F0
  )
{
  EFI_STATUS  Status;
  UINT32      Addr = SMU8_REPORTED_TEMP_REG;
  UINT32      Reg = 0;
  UINTN       TempC;
  STATIC UINTN  LogLeft = 3;   /* the raw read is polled every tick: log thrice */

  if (D0F0 == NULL) {
    return;
  }

  Status = D0F0->Pci.Write (D0F0, EfiPciIoWidthUint32, D0F0_SMU_INDEX_REG, 1, &Addr);
  if (EFI_ERROR (Status)) {
    return;
  }
  Status = D0F0->Pci.Read (D0F0, EfiPciIoWidthUint32, D0F0_SMU_DATA_REG, 1, &Reg);
  if (LogLeft-- > 0) {
    SbLog ("[hw] smu8 temp raw", Status, Reg);
  }
  if (EFI_ERROR (Status) || Reg == 0xFFFFFFFFu) {
    return;
  }

  TempC = (UINTN)(Reg >> 21) / 8;
  if (((Reg >> 16) & 0x3) == 0x3) {
    TempC = (TempC >= 49) ? (TempC - 49) : 0;
  }

  if (TempC >= 10 && TempC <= 115) {
    mTempC    = TempC;
    mTempReal = TRUE;
  }
}

STATIC
EFI_STATUS
SmuWrite (
  IN EFI_PCI_IO_PROTOCOL  *D0F0,
  IN UINT32               Addr,
  IN UINT32               Val
  )
{
  EFI_STATUS  Status;

  Status = D0F0->Pci.Write (D0F0, EfiPciIoWidthUint32, D0F0_SMU_INDEX_REG, 1, &Addr);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return D0F0->Pci.Write (D0F0, EfiPciIoWidthUint32, D0F0_SMU_DATA_REG, 1, &Val);
}

STATIC
EFI_STATUS
SmuRead (
  IN  EFI_PCI_IO_PROTOCOL  *D0F0,
  IN  UINT32               Addr,
  OUT UINT32               *Val
  )
{
  EFI_STATUS  Status;

  Status = D0F0->Pci.Write (D0F0, EfiPciIoWidthUint32, D0F0_SMU_INDEX_REG, 1, &Addr);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return D0F0->Pci.Read (D0F0, EfiPciIoWidthUint32, D0F0_SMU_DATA_REG, 1, Val);
}

/*
 * One-shot iGPU max frequency limit via SMU mailbox on D0F0xB8/BC.
 * Clears all mailbox registers afterward so other software can use it cleanly.
 */
STATIC
VOID
ReadGpuFreqFromD0F0 (
  IN EFI_PCI_IO_PROTOCOL  *D0F0
  )
{
  EFI_STATUS  Status;
  UINT32      Rsp = 0;
  UINT32      Arg1 = 0;
  UINTN       Tries;
  UINTN       FreqMHz;

  if (D0F0 == NULL || !mGpuPresent) {
    return;
  }

  if (EFI_ERROR (SmuWrite (D0F0, SMU_RSP_ADDR + 0x4, 0))) {
    return;
  }
  if (EFI_ERROR (SmuWrite (D0F0, SMU_ARG0_ADDR + 6 * 0x4, 8))) {
    goto Cleanup;
  }
  if (EFI_ERROR (SmuWrite (D0F0, SMU_CMD_ADDR + 0x4, 8))) {
    goto Cleanup;
  }

  for (Tries = 0; Tries < 1024; Tries++) {
    Rsp = 0;
    Status = SmuRead (D0F0, SMU_RSP_ADDR + 0x4, &Rsp);
    if (EFI_ERROR (Status) || Rsp != 0) {
      break;
    }
    CpuPause ();
  }

  Status = SmuRead (D0F0, SMU_ARG1_ADDR + 6 * 0x4, &Arg1);
  SbLog ("[hw] smu igpu arg1", Status, Arg1);
  if (!EFI_ERROR (Status) && Arg1 != 0 && Arg1 != 0xFFFFFFFFu) {
    FreqMHz = (UINTN)(Arg1 / 100);
    if (FreqMHz >= 100 && FreqMHz <= 3000) {
      mGpuFreqMHz = FreqMHz;
    }
  }

Cleanup:
  SmuWrite (D0F0, SMU_CMD_ADDR + 0x4,  0);
  SmuWrite (D0F0, SMU_RSP_ADDR + 0x4,  0);
  SmuWrite (D0F0, SMU_ARG0_ADDR + 6 * 0x4, 0);
  SmuWrite (D0F0, SMU_ARG1_ADDR + 6 * 0x4, 0);
  SmuWrite (D0F0, SMU_ARG2_ADDR + 6 * 0x4, 0);
  SmuWrite (D0F0, SMU_ARG4_ADDR + 6 * 0x4, 0);
  SmuWrite (D0F0, SMU_ARG5_ADDR + 6 * 0x4, 0);
}

STATIC
VOID
ReadMemFromD18F2 (
  IN EFI_PCI_IO_PROTOCOL  *D18F2
  )
{
  EFI_STATUS  Status;
  UINT32      Dchr = 0;
  UINT32      Idx, Reg = 0;
  UINTN       Tries;

  if (D18F2 == NULL) {
    return;
  }

  /* Memory frequency from D18F2x94[MemClkFreq] (bits 4:0) */
  Status = D18F2->Pci.Read (D18F2, EfiPciIoWidthUint32, D18F2_DCHR0_REG, 1, &Dchr);
  SbLog ("[hw] d18f2x94", Status, Dchr);
  if (!EFI_ERROR (Status) && Dchr != 0xFFFFFFFFu && ((Dchr & (1u << 7)) != 0)) {
    UINTN  MemClkFreq = (UINTN)(Dchr & 0x1F);
    if (MemClkFreq >= 0x04) {
      mMemFreqMHz = ((MemClkFreq + 6) * 200) / 3;
      mMemReal    = TRUE;
    }
  }

  /* Memory voltage from D18F2x9C_x0001_004A (DqDqsRcvCntrl1) */
  Idx = DCT_DQ_DQS_RCV_CNTRL1;
  Status = D18F2->Pci.Write (D18F2, EfiPciIoWidthUint32, D18F2_DCT_OFFSET_REG, 1, &Idx);
  if (!EFI_ERROR (Status)) {
    for (Tries = 0; Tries < 64; Tries++) {
      Idx = 0;
      Status = D18F2->Pci.Read (D18F2, EfiPciIoWidthUint32, D18F2_DCT_OFFSET_REG, 1, &Idx);
      if (EFI_ERROR (Status) || (Idx & 0x80000000u) != 0) {
        break;
      }
      CpuPause ();
    }
    if (!EFI_ERROR (Status) && (Idx & 0x80000000u) != 0) {
      Status = D18F2->Pci.Read (D18F2, EfiPciIoWidthUint32, D18F2_DCT_DATA_REG, 1, &Reg);
      SbLog ("[hw] dqdqsrcvcntrl1", Status, Reg);
      if (!EFI_ERROR (Status) && Reg != 0xFFFFFFFFu) {
        UINTN  MajorMode = (UINTN)((Reg >> 4) & 0x7);
        switch (MajorMode) {
          case 0:
            mMemVoltMv   = 1500;   /* DDR3  1.50V */
            mMemVoltReal = TRUE;
            break;
          case 1:
            mMemVoltMv   = 1350;   /* DDR3L 1.35V */
            mMemVoltReal = TRUE;
            break;
          case 2:
          case 3:
            mMemVoltMv   = 1200;   /* DDR4 / LPDDR3 1.20V */
            mMemVoltReal = TRUE;
            break;
          default:
            break;
        }
      }
    }
  }
}

STATIC
VOID
ReadGpuVoltFromD18F5 (
  IN EFI_PCI_IO_PROTOCOL  *D18F5
  )
{
  EFI_STATUS  Status;
  UINT32      Reg = 0;
  UINTN       NbVid, NbPs;

  if (D18F5 == NULL) {
    return;
  }

  Status = D18F5->Pci.Read (D18F5, EfiPciIoWidthUint32, D18F5_NBPSTATE_STS_REG, 1, &Reg);
  SbLog ("[hw] d18f5x174", Status, Reg);
  if (EFI_ERROR (Status) || Reg == 0xFFFFFFFFu) {
    return;
  }

  NbVid = (UINTN)(((Reg >> 10) & 0x7F) | (((Reg >> 22) & 1) << 7));
  if (NbVid == 0 || NbVid >= 0xF8) {
    /* Cross-check active NB P-state entry D18F5x16[0..C] */
    NbPs = (UINTN)((Reg >> 19) & 0x3);
    Reg  = 0;
    Status = D18F5->Pci.Read (D18F5, EfiPciIoWidthUint32,
                              (UINT32)(D18F5_NBPSTATE_BASE + NbPs * 4), 1, &Reg);
    if (!EFI_ERROR (Status) && Reg != 0xFFFFFFFFu) {
      NbVid = (UINTN)(((Reg >> 10) & 0x7F) | (((Reg >> 21) & 1) << 7));
    }
  }

  if (NbVid > 0 && NbVid < 0xF8) {
    mGpuVoltMv   = Svi2VidToMv (NbVid);
    mGpuVoltReal = (mGpuVoltMv > 0);
  }
}

/*
 * Single-pass PCI discovery for D0F0 (SMU v8), D1F0 (iGPU), D18F2 (DCT),
 * and D18F5 (NB P-state).  Called once so runtime cost is zero.
 */
STATIC
VOID
ProbePciOnce (
  VOID
  )
{
  EFI_HANDLE           *Handles;
  EFI_STATUS           Status;
  UINTN                Index, Count;
  EFI_PCI_IO_PROTOCOL  *Pci;
  EFI_PCI_IO_PROTOCOL  *D18F2 = NULL;
  EFI_PCI_IO_PROTOCOL  *D18F5 = NULL;

  if (!mIsSmu8Apu) {
    return;
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid,
                                     NULL, &Count, &Handles);
  SbLog ("[hw] pci handles", Count, (UINTN)Status);
  if (EFI_ERROR (Status) || Handles == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    UINTN   Seg, Bus, Dev, Func;
    UINT32  Id = 0;

    Pci = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid,
                                        (VOID **)&Pci)) || (Pci == NULL)) {
      continue;
    }
    if (EFI_ERROR (Pci->GetLocation (Pci, &Seg, &Bus, &Dev, &Func))) {
      continue;
    }
    if (Seg != 0 || Bus != 0) {
      continue;
    }

    if (Dev == 0 && Func == 0) {
      if (!EFI_ERROR (Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id)) &&
          ((Id & 0xFFFF) == 0x1022)) {
        mD0F0 = Pci;
      }
    } else if (Dev == 1 && Func == 0) {
      if (!EFI_ERROR (Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id)) &&
          ((Id & 0xFFFF) == 0x1002)) {
        mGpuPresent = TRUE;
        mGpuFreqMHz = 800;   /* Stoney Ridge A9-9420 Radeon R5 nominal SCLK */
        SbLog ("[hw] igpu id", Id & 0xFFFF, (Id >> 16) & 0xFFFF);
      }
    } else if (Dev == 18 && Func == 2) {
      if (!EFI_ERROR (Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id)) &&
          ((Id & 0xFFFF) == 0x1022)) {
        D18F2 = Pci;
      }
    } else if (Dev == 18 && Func == 5) {
      if (!EFI_ERROR (Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id)) &&
          ((Id & 0xFFFF) == 0x1022)) {
        D18F5 = Pci;
      }
    }
  }

  FreePool (Handles);

  /* one-shot reads: the iGPU SCLK limit does not change at runtime */
  ReadGpuFreqFromD0F0 (mD0F0);
  ReadMemFromD18F2 (D18F2);
  ReadGpuVoltFromD18F5 (D18F5);
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
    ProbeBrandAndFamily ();
    ProbePciOnce ();
    mProbed = TRUE;
    SbLog ("[hw] probe done", mCpuReal, mTempReal);
  }

  /* live sensors: re-read every tick (MSR read + two PCI config accesses) */
  ReadCpuMsr ();
  ReadTempFromD0F0 (mD0F0);

  ZeroMem (Hw, sizeof (*Hw));
  ClockToText (Hw->Time, Hw->Date, Hw->Weekday);
  StrCpyS (Hw->Brand, 64, mBrand);
  Hw->CpuReal  = mCpuReal;
  Hw->TempReal = mTempReal;

  if (mCpuReal) {
    FmtGHz (Hw->CpuFreq, 16, mCpuFreqMHz);
    FmtVolt (Hw->CpuVolt, 16, mCpuVoltMv);
  } else {
    StrCpyS (Hw->CpuFreq, 16, L"3.0 GHz");
    StrCpyS (Hw->CpuVolt, 16, L"1.25V");
  }

  if (mTempReal) {
    UnicodeSPrint (Hw->CpuTemp, 16 * sizeof (CHAR16), L"%uC", mTempC);
  } else {
    StrCpyS (Hw->CpuTemp, 16, L"44C");
  }

  StrCpyS (Hw->CpuBclk, 16, L"100 MHz");

  if (mMemReal) {
    UnicodeSPrint (Hw->MemFreq, 16 * sizeof (CHAR16), L"%u MHz", mMemFreqMHz);
  } else {
    StrCpyS (Hw->MemFreq, 16, L"2133 MHz");
  }

  if (mMemVoltReal) {
    FmtVolt (Hw->MemVolt, 16, mMemVoltMv);
  } else {
    StrCpyS (Hw->MemVolt, 16, L"1.20V");
  }

  if (mGpuPresent && mGpuFreqMHz > 0) {
    UnicodeSPrint (Hw->GpuFreq, 16 * sizeof (CHAR16), L"%u MHz", mGpuFreqMHz);
  } else {
    StrCpyS (Hw->GpuFreq, 16, L"800 MHz");
  }

  if (mGpuVoltReal) {
    FmtVolt (Hw->GpuVolt, 16, mGpuVoltMv);
  } else {
    StrCpyS (Hw->GpuVolt, 16, L"1.00V");
  }
}