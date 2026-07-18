#include <Uefi.h>

#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

STATIC CONST EFI_GUID gMicrosoftVendorGuid =
    {0x77fa9abd, 0x0359, 0x4d32, {0xbd, 0x60, 0x28, 0xf4, 0xe7, 0x8f, 0x78, 0x4b}};

STATIC CONST CHAR16 mVbsVarName[] = L"VbsPolicyDisabled";
#define VBS_VAR_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

STATIC CONST CHAR16 mWinBootFullPath[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

STATIC
VOID
DisableVBS(
    VOID
)
{
    EFI_STATUS Status;
    UINT8 TargetValue = 1;
    UINT8 CurrentValue = 0;
    UINTN Size = sizeof(CurrentValue);

    Status = gRT->GetVariable((CHAR16 *)mVbsVarName,
                             (EFI_GUID *)&gMicrosoftVendorGuid,
                             NULL, &Size, &CurrentValue);

    if (!EFI_ERROR(Status) && CurrentValue == TargetValue) {
        Print(L"[Info] VBS already disabled.\n");
        return;
    }

    Status = gRT->SetVariable((CHAR16 *)mVbsVarName,
                             (EFI_GUID *)&gMicrosoftVendorGuid,
                             VBS_VAR_ATTR,
                             sizeof(TargetValue),
                             &TargetValue);

    if (EFI_ERROR(Status)) {
        gRT->SetVariable((CHAR16 *)mVbsVarName, (EFI_GUID *)&gMicrosoftVendorGuid, 0, 0, NULL);
        Status = gRT->SetVariable((CHAR16 *)mVbsVarName,
                                 (EFI_GUID *)&gMicrosoftVendorGuid,
                                 VBS_VAR_ATTR,
                                 sizeof(TargetValue),
                                 &TargetValue);
    }

    Print(EFI_ERROR(Status)
          ? L"[Error] VBS Disable failed: %r\n"
          : L"[Info] VBS Disable succeeded: %r\n",
          Status);
}

// 尝试连接控制器，很多固件需要这步才能把磁盘/FS驱动挂上来
STATIC
VOID
ConnectAllControllersBestEffort(
    VOID
)
{
    EFI_STATUS Status;
    EFI_HANDLE *Handles = NULL;
    UINTN Count = 0;
    UINTN i;

    Status = gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &Count, &Handles);
    if (EFI_ERROR(Status) || Handles == NULL) {
        Print(L"[Diag] LocateHandleBuffer(AllHandles) failed: %r\n", Status);
        return;
    }

    for (i = 0; i < Count; i++) {
        // ignore error
        gBS->ConnectController(Handles[i], NULL, NULL, TRUE);
    }

    FreePool(Handles);
    Print(L"[Diag] ConnectController(all) done.\n");
}

STATIC
EFI_STATUS
TryStartBootmgfwOnFsHandle(
    IN EFI_HANDLE ImageHandle,
    IN EFI_HANDLE FsHandle
)
{
    EFI_STATUS Status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_DEVICE_PATH_PROTOCOL *Dp = NULL;
    EFI_HANDLE WinImage = NULL;

    Status = gBS->HandleProtocol(FsHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = Sfs->OpenVolume(Sfs, &Root);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = Root->Open(Root, &File, (CHAR16 *)mWinBootFullPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return EFI_NOT_FOUND;
    }

    File->Close(File);
    Root->Close(Root);

    Print(L"[Boot] Found: %s\n", mWinBootFullPath);

    Dp = FileDevicePath(FsHandle, (CHAR16 *)mWinBootFullPath);
    if (Dp == NULL) {
        Print(L"[Error] FileDevicePath failed.\n");
        return EFI_OUT_OF_RESOURCES;
    }

    Status = gBS->LoadImage(FALSE, ImageHandle, Dp, NULL, 0, &WinImage);
    Print(L"[Boot] LoadImage returned: %r\n", Status);
    if (EFI_ERROR(Status)) {
        FreePool(Dp);
        return Status;
    }

    // 关键诊断：StartImage 的返回码
    Status = gBS->StartImage(WinImage, NULL, NULL);
    Print(L"[Boot] StartImage returned: %r\n", Status);

    FreePool(Dp);
    return Status;
}

STATIC
EFI_STATUS
BootWindowsBySearchingAllFileSystems(
    IN EFI_HANDLE ImageHandle
)
{
    EFI_STATUS Status;
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;
    UINTN i;

    Print(L"[Boot] Searching all file systems for %s\n", mWinBootFullPath);

    Status = gBS->LocateHandleBuffer(ByProtocol,
                                     &gEfiSimpleFileSystemProtocolGuid,
                                     NULL,
                                     &HandleCount,
                                     &Handles);
    Print(L"[Diag] LocateHandleBuffer(SimpleFileSystem) => %r, Count=%u\n", Status, (UINT32)HandleCount);

    if (EFI_ERROR(Status) || Handles == NULL || HandleCount == 0) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < HandleCount; i++) {
        Print(L"[Diag] Try FS handle %u/%u ...\n", (UINT32)(i+1), (UINT32)HandleCount);
        Status = TryStartBootmgfwOnFsHandle(ImageHandle, Handles[i]);

        // 如果成功交接，通常不会返回；如果返回 EFI_SUCCESS 也说明它返回了（异常但我们认为它成功过）
        if (!EFI_ERROR(Status)) {
            FreePool(Handles);
            return Status;
        }

        // 继续尝试下一个分区（很关键，避免某个分区有同名文件但不可启动）
        Print(L"[Diag] This handle failed: %r\n", Status);
    }

    FreePool(Handles);
    return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
)
{
    EFI_STATUS Status;
    UINTN Index;
    EFI_INPUT_KEY Key;

    DisableVBS();

    // 连接所有控制器，提升找到硬盘ESP的概率
    ConnectAllControllersBestEffort();

    Status = BootWindowsBySearchingAllFileSystems(ImageHandle);
    Print(L"[Info] Boot attempt finished: %r\n", Status);

    Print(L"\nPress any key to exit...\n");
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
    gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    return EFI_SUCCESS;
}
