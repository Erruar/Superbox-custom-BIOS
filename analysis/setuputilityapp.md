# SetupUtilityApp.efi — reverse-engineering notes

Source: `bios-dumps/modules/Setup/SetupUtilityApp.efi` (6,912 B, X64 EFI application).
Disassembly: Binary Ninja database (MCP `binja-codemode-mcp`), full listing in
`bios-dumps/modules/Setup/SetupUtilityApp.efi.c`.

## Role in the H2O setup chain

```
BdsDxe (hotkey/F2) -> SetupUtilityApp (entry app, THIS MODULE)
    -> EFI_FORM_BROWSER2_PROTOCOL.SendForm()
        -> SetupBrowser (Insyde H2O display engine, does all drawing)
```

SetupUtilityApp is a thin, standards-based "launcher/browser driver":
it locates the form sets in the HII database, injects a custom exit form,
then calls the EDK2 FormBrowser2. All pixel drawing happens inside
SetupBrowser. **This app is the module we replace with our own GUI.**

## Entry point flow

```c
_ModuleEntry (0x4002c0)
  sub_400324   init   (globals + protocol lookup + formset collection)
  sub_400cd8   main   (guard install -> collect -> inject -> SendForm)
  sub_40037c   exit   (close events, notify H2O protocol vtbl[2]/[3])
```

### Init (`sub_400324`)

| Step | Address | What it does |
|---|---|---|
| 1 | `sub_400f8c` | cache `gHandle`, `gST`, `gBS` |
| 2 | `sub_40101c` | cache `gRT` |
| 3 | `sub_401088` | `LocateProtocol`: HII_STRING, HII_DATABASE, HII_CONFIG_ROUTING, HII_FONT, HII_IMAGE |
| 4 | `sub_4011c0` | SMM: `LocateProtocol(EFI_SMM_BASE2)` -> `InMm()`; else check `LoadedImage` type==5 (`data_401880`); `LocateProtocol(1C2E4602…)` -> call vtbl[2] (+0x10) if present, else register `RegisterProtocolNotify` (event `sub_40116c`) |
| 5 | `sub_4014c8` | SMM variable path: `gMmst` + `SMM_VARIABLE` locate/notify; non-SMM path: `CreateEventEx(gEfiEventReadyToBootGuid (55F0AB27…), sub_401494)` -> on ReadyToBoot set `data_401858 = 1` |

### Main (`sub_400cd8`)

1. **Single-instance guard**: `LocateProtocol(E7A97370-00FF-4BBA-BBA0-D0BC200CCE98)`;
   if present → `EFI_ACCESS_DENIED`. Installs the protocol (4-byte flag)
   for the duration of setup, reinstalls (flag=1) and uninstalls on exit.
2. **Collect form sets** (`sub_400634`):
   `HiiDatabase->ListPackageLists(HII_PACK_ANY)` → for each handle
   `ExportPackageLists` → walk packages, `FormPacket` (type 0x02),
   `sub_4003b0` walks IFR opcodes looking for `EFI_IFR_FORM_SET_OP` (0x0E)
   and compares its GUID against the table at `data_401770` (6 GUIDs).
   Output: zero-terminated array of matching `EFI_HII_HANDLE`s.
3. **Inject exit form** (`sub_4008b4` + `sub_400820`):
   builds a 0x19-byte string/opcode blob
   (formset GUID `9F85453E-4989-3BAD-4A84-0791AF3A2F03`,
   opcodes `0x29` = EFI_IFR_ACTION? with key/flags 0xf0d2/0x1ffff),
   splices it into the first FormPacket of every collected handle
   (patching packet length), then `HiiDatabase->UpdatePackageList`.
4. **Run browser**: `LocateProtocol(E1C17F13-BA6B-42E5-985A-B4187CA01A3F)`
   (H2O "browser ready" marker) →
   `FormBrowser2->SendForm(handles, FormSetGuid = 9F85453E-…, 1, NULL, &ActionRequest)`.
5. **Post**: `ActionRequest == EFI_BROWSER_ACTION_REQUEST_RESET` →
   `RaiseTPL` + `gRT->ResetSystem(EfiResetCold)`. Then
   `ConOut->SetAttribute(0)` + `ClearScreen`, uninstall guard protocol.

## Collected form-set GUIDs (`data_401770`, 6 entries)

```
C1E0B01A-607E-4B75-B8BB-0631ECFAACF2
C6D4769E-7F48-4D2A-98E9-87ADCCF35CCC
5204F764-DF25-48A2-B337-9EC122B85E0D
A6712873-925F-46C6-90B4-A40F86A0917B
2D068309-12AC-45AB-9600-9187513CCDD8
B6936426-FB04-4A7B-AA51-FD49397CDC01
```

Injected/exit formset: `9F85453E-4989-3BAD-4A84-0791AF3A2F03`.
These are the standard Insyde H2O Main/Advanced/Boot/Security/... forms;
they are visible to us at runtime through the HII database, no patching needed.

## Protocol GUID table (from `.data`)

| GUID | Name | Standard? |
|---|---|---|
| EF9FC172-A1B2-4693-B327-6D32FC416042 | EFI_HII_DATABASE_PROTOCOL | yes |
| 0FD96974-23AA-4CDC-B9CB-98D17750322A | EFI_HII_STRING_PROTOCOL | yes |
| 31A6406A-6BDF-4E46-B2A2-EBAA89C40920 | EFI_HII_IMAGE_PROTOCOL | yes |
| E9CA4775-8657-47FC-97E7-7ED65A084324 | EFI_HII_FONT_PROTOCOL | yes |
| 587E72D7-CC50-4F79-8209-CA291FC1A10F | EFI_HII_CONFIG_ROUTING_PROTOCOL | yes |
| B9D4C360-BCFB-4F9B-9298-53C136982258 | EFI_FORM_BROWSER2_PROTOCOL | yes |
| F4CCBFB7-F6E0-47FD-9DD4-10A8F150C191 | EFI_SMM_BASE2 (MmBase) | yes |
| ED32D533-99E6-4209-9CC0-2D72CDD998A7 | EFI_SMM_VARIABLE_PROTOCOL | yes |
| 5B1B31A1-9562-11D2-8E3F-00A0C969723B | EFI_LOADED_IMAGE_PROTOCOL | yes |
| 55F0AB27-B8B1-4C26-8048-748F37BAA2DF | gEfiEventReadyToBootGuid | yes (MdePkg) |
| FE3542FE-C1D3-4EF8-657C-8048606FF670 | "bios" (used as protocol GUID here) | Insyde |
| E7A97370-00FF-4BBA-BBA0-D0BC200CCE98 | setup single-instance guard | Insyde private |
| 1C2E4602-E3BA-4B07-E3B8-3555A51C613A | notify + vtbl[2]/[3] enter/leave | Insyde private |
| E1C17F13-BA6B-42E5-985A-B4187CA01A3F | "browser ready" marker | Insyde private |

## Conclusions for our replacement app

- The app talks to **stock protocols only** (HII DB, ConfigRouting,
  FormBrowser2, SMM variable). All of these exist on this firmware, proven
  by this module.
- A replacement GUI app needs to:
  1. enumerate HII handles (`ListPackageLists` + `ExportPackageLists`),
  2. parse IFR (form packets: form set GUID + string packages via
     `HiiString->NewString`), 
  3. render on GOP directly (cursor, 4K scaling — ours to implement),
  4. read/write settings through `HiiConfigRouting` (config XML) or the
     per-driver `HiiConfigAccess`,
  5. commit + cold-reset exactly like `sub_400bc8` does.
- No dependency on the Insyde display engine, no IFR rewriting required.
  SetupBrowser (the stock drawing layer) stays untouched and is simply not
  called by us.
- The 6 formset GUIDs are a convenience list; enumerating all package
  lists (type 0 = `HII_PACK_ANY`) covers everything automatically.
