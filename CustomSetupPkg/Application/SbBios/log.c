/*
 * log.c - field tracing: serial port plus SBXLOG.TXT on every FAT volume.
 *
 * Under QEMU the serial UART is captured by the test harness.  On the target
 * machine (a TV-box without a usable serial console) the only observable
 * output is the screen, so the same lines are appended to a file on the
 * boot USB stick; SbLogInit opens/creates one file per mounted volume.
 */

#include "SbBios.h"

#define LOG_FILE_NAME  L"sbxlog.txt"
#define MAX_LOG_VOLS   8

STATIC EFI_FILE_PROTOCOL  *mLogFile[MAX_LOG_VOLS];
STATIC UINTN              mLogSeq;

EFI_STATUS
SbLogInit (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  UINTN       Index, Count;

  if (mLogFile[0] != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
                   &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
    EFI_FILE_PROTOCOL                *Root;
    EFI_FILE_PROTOCOL                *File;

    if (Index >= MAX_LOG_VOLS) {
      break;
    }
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
           &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) || (Fs == NULL)) {
      continue;
    }
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    File = NULL;
    Status = Root->Open (Root, &File, LOG_FILE_NAME,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (Status == EFI_NOT_FOUND) {
      Status = Root->Open (Root, &File, LOG_FILE_NAME,
                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                           EFI_FILE_MODE_CREATE, 0);
    }
    if (!EFI_ERROR (Status)) {
      /* append mode: seek past the existing content */
      File->SetPosition (File, (UINT64)-1);
      mLogFile[Index] = File;
    }
    Root->Close (Root);
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

VOID
SbLogStr (
  IN CONST CHAR8  *Line
  )
{
  UINTN  Index;
  UINTN  Len = AsciiStrLen (Line);

  SerialPortWrite ((UINT8 *)Line, Len - 1);

  for (Index = 0; Index < MAX_LOG_VOLS; Index++) {
    if (mLogFile[Index] != NULL) {
      mLogFile[Index]->Write (mLogFile[Index], &Len, (CHAR8 *)Line);
      mLogFile[Index]->Flush (mLogFile[Index]);
    }
  }
}

VOID
SbLog (
  IN CONST CHAR8  *Tag,
  IN UINTN        V1,
  IN UINTN        V2
  )
{
  CHAR8  Line[128];

  AsciiSPrint (Line, sizeof (Line), "[%5d] %a %u 0x%x\r\n",
               (UINT32)mLogSeq++, Tag, (UINT32)V1, (UINT32)V2);
  SbLogStr (Line);
}

VOID
SbLog6 (
  IN CONST CHAR8  *Tag,
  IN UINTN        V1,
  IN UINTN        V2,
  IN UINTN        V3,
  IN UINTN        V4,
  IN UINTN        V5,
  IN UINTN        V6
  )
{
  CHAR8  Line[160];

  AsciiSPrint (Line, sizeof (Line), "[%5d] %a %u %u 0x%x 0x%x 0x%x 0x%x\r\n",
               (UINT32)mLogSeq++, Tag, (UINT32)V1, (UINT32)V2, (UINT32)V3,
               (UINT32)V4, (UINT32)V5, (UINT32)V6);
  SbLogStr (Line);
}
