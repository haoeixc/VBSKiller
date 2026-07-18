#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

#include <Protocol/SimpleFileSystem.h>

STATIC CONST EFI_GUID gMicrosoftVendorGuid =
    {0x77fa9abd, 0x0359, 0x4d32, {0xbd, 0x60, 0x28, 0xf4, 0xe7, 0x8f, 0x78, 0x4b}};

STATIC CONST CHAR16 mVbsVarName[] = L"VbsPolicyDisabled";
#define VBS_VAR_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

STATIC CONST CHAR16 mWinBootFile[] = L"BOOTMGFW.EFI";
#define WIN_BOOT_FILE_LEN 12

// 引导修复用：Windows Boot Manager 标准路径
STATIC CONST CHAR16 mWinBootFullPath[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

STATIC
BOOLEAN
IsWindowsBootFile(
    IN CHAR16 *PathName
)
{
    UINTN Len;
    UINTN Index;
    CHAR16 CharA, CharB;

    if (PathName == NULL) {
        return FALSE;
    }

    Len = StrLen(PathName);
    if (Len < WIN_BOOT_FILE_LEN) {
        return FALSE;
    }

    PathName += (Len - WIN_BOOT_FILE_LEN);

    for (Index = 0; Index < WIN_BOOT_FILE_LEN; Index++) {
        CharA = PathName[Index];
        CharB = mWinBootFile[Index];

        if (CharA >= L'a' && CharA <= L'z') {
            CharA -= (L'a' - L'A');
        }
        if (CharA != CharB) {
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * DisableVBS：保持原逻辑不动（按你的要求不改）
 */
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
                             NULL,
                             &Size,
                             &CurrentValue);
    if (!EFI_ERROR(Status) && CurrentValue == TargetValue) {
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

    if (EFI_ERROR(Status)) {
        Print(L"[Error] VBS Disable failed: %r\n", Status);
    } else {
        Print(L"[Info] VBS Disable succeeded: %r\n", Status);
    }
}

/* =========================
 * 仅修复引导相关：新增函数
 * ========================= */

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
        return;
    }

    for (i = 0; i < Count; i++) {
        // ignore errors
        gBS->ConnectController(Handles[i], NULL, NULL, TRUE);
    }

    FreePool(Handles);
}

STATIC
EFI_STATUS
TryStartBootmgfwOnHandle(
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

    // 检查文件是否存在
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
        return EFI_OUT_OF_RESOURCES;
    }

    Status = gBS->LoadImage(FALSE, ImageHandle, Dp, NULL, 0, &WinImage);
    Print(L"[Boot] LoadImage: %r\n", Status);

    if (!EFI_ERROR(Status)) {
        Status = gBS->StartImage(WinImage, NULL, NULL);
        Print(L"[Boot] StartImage returned: %r\n", Status);
    }

    FreePool(Dp);
    return Status;
}

STATIC
EFI_STATUS
BootWindowsDirect(
    IN EFI_HANDLE ImageHandle
)
{
    EFI_STATUS Status;
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;
    UINTN i;

    Status = gBS->LocateHandleBuffer(ByProtocol,
                                     &gEfiSimpleFileSystemProtocolGuid,
                                     NULL,
                                     &HandleCount,
                                     &Handles);
    if (EFI_ERROR(Status) || Handles == NULL || HandleCount == 0) {
        Print(L"[Error] LocateHandleBuffer(SimpleFileSystem) failed: %r\n", Status);
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < HandleCount; i++) {
        Status = TryStartBootmgfwOnHandle(ImageHandle, Handles[i]);
        if (!EFI_ERROR(Status)) {
            FreePool(Handles);
            return Status; // 正常交接不会返回
        }
    }

    FreePool(Handles);
    Print(L"[Error] bootmgfw.efi not found on any filesystem.\n");
    return EFI_NOT_FOUND;
}

/*
 * BootWindows：保留原来的“找 Windows Boot Manager”逻辑与输出，
 * 但把 EfiBootManagerBoot 替换为直接 LoadImage/StartImage，修复卡死。
 */
STATIC
VOID
BootWindows(
    IN EFI_HANDLE ImageHandle
)
{
    EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions;
    UINTN BootOptionCount;
    UINTN Index;
    EFI_DEVICE_PATH_PROTOCOL *Node;
    FILEPATH_DEVICE_PATH *FilePathNode;
    BOOLEAN Found;

    BootOptions = EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
    if (BootOptions == NULL) {
        ConnectAllControllersBestEffort();
        BootWindowsDirect(ImageHandle);
        return;
    }

    for (Index = 0; Index < BootOptionCount; Index++) {
        if ((BootOptions[Index].Attributes & LOAD_OPTION_ACTIVE) == 0) {
            continue;
        }

        Found = FALSE;
        Node = BootOptions[Index].FilePath;

        while (!IsDevicePathEnd(Node)) {
            if ((DevicePathType(Node) == MEDIA_DEVICE_PATH) &&
                (DevicePathSubType(Node) == MEDIA_FILEPATH_DP)) {
                FilePathNode = (FILEPATH_DEVICE_PATH *)Node;
                if (IsWindowsBootFile(FilePathNode->PathName)) {
                    Found = TRUE;
                    break;
                }
            }
            Node = NextDevicePathNode(Node);
        }

        if (Found) {
            Print(L"[Boot] Windows found: %s\n", BootOptions[Index].Description);

            // 关键修复：不调用 EfiBootManagerBoot（你机器上会卡住）
            EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);

            // 为了更大兼容性，先连接控制器再找 ESP
            ConnectAllControllersBestEffort();

            BootWindowsDirect(ImageHandle);
            return;
        }
    }

    Print(L"[Error] Windows boot loader not found.\n");
    EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);

    // 仅引导兜底
    ConnectAllControllersBestEffort();
    BootWindowsDirect(ImageHandle);
}

EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
)
{
    UINTN Index;
    EFI_INPUT_KEY Key;

    DisableVBS();
    BootWindows(ImageHandle);

    Print(L"\nPress any key to exit...\n");
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
    gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    return EFI_SUCCESS;
}
