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

#include <Protocol/DevicePath.h>

STATIC CONST EFI_GUID gMicrosoftVendorGuid =
    {0x77fa9abd, 0x0359, 0x4d32, {0xbd, 0x60, 0x28, 0xf4, 0xe7, 0x8f, 0x78, 0x4b}};

STATIC CONST CHAR16 mVbsVarName[] = L"VbsPolicyDisabled";

// 原项目少了 RUNTIME_ACCESS，某些固件/场景下不稳，补上
#define VBS_VAR_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

STATIC CONST CHAR16 mWinBootFile[] = L"BOOTMGFW.EFI";
#define WIN_BOOT_FILE_LEN 12

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
        Print(L"[Info] VBS already disabled.\n");
        return;
    }

    Status = gRT->SetVariable((CHAR16 *)mVbsVarName,
                             (EFI_GUID *)&gMicrosoftVendorGuid,
                             VBS_VAR_ATTR,
                             sizeof(TargetValue),
                             &TargetValue);

    if (EFI_ERROR(Status)) {
        // 某些固件需要先 delete 再 set
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

// 从某个 BootOption 的“设备路径部分”拼出 \EFI\Microsoft\Boot\bootmgfw.efi 并启动
STATIC
EFI_STATUS
StartBootmgfwFromBootOption(
    IN EFI_HANDLE ImageHandle,
    IN EFI_BOOT_MANAGER_LOAD_OPTION *Opt
)
{
    EFI_STATUS Status;
    EFI_HANDLE WinImage = NULL;

    EFI_DEVICE_PATH_PROTOCOL *Node;
    EFI_DEVICE_PATH_PROTOCOL *DevicePartEnd = NULL;
    EFI_DEVICE_PATH_PROTOCOL *DevicePart = NULL;
    EFI_DEVICE_PATH_PROTOCOL *WinPath = NULL;
    EFI_DEVICE_PATH_PROTOCOL *FilePart = NULL;

    if (Opt == NULL || Opt->FilePath == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // 找到第一个 FILEPATH 节点，把它作为截断点：之前是“设备路径”，之后是文件路径
    Node = Opt->FilePath;
    while (!IsDevicePathEnd(Node)) {
        if (DevicePathType(Node) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(Node) == MEDIA_FILEPATH_DP) {
            DevicePartEnd = Node;
            break;
        }
        Node = NextDevicePathNode(Node);
    }

    if (DevicePartEnd != NULL) {
        UINTN Size = (UINTN)((UINT8*)DevicePartEnd - (UINT8*)Opt->FilePath);
        DevicePart = AllocateCopyPool(Size + END_DEVICE_PATH_LENGTH, Opt->FilePath);
        if (DevicePart == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        SetDevicePathEndNode((EFI_DEVICE_PATH_PROTOCOL *)((UINT8*)DevicePart + Size));
    } else {
        // 没有 FILEPATH 节点就退化为整条路径（不一定好用，但比什么都不做强）
        DevicePart = DuplicateDevicePath(Opt->FilePath);
        if (DevicePart == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
    }

    // 构造文件路径节点
    FilePart = FileDevicePath(NULL, (CHAR16 *)mWinBootFullPath);
    if (FilePart == NULL) {
        FreePool(DevicePart);
        return EFI_OUT_OF_RESOURCES;
    }

    // 拼接设备路径 + 文件路径
    WinPath = AppendDevicePath(DevicePart, FilePart);

    FreePool(DevicePart);
    FreePool(FilePart);

    if (WinPath == NULL) {
        Print(L"[Error] AppendDevicePath failed.\n");
        return EFI_OUT_OF_RESOURCES;
    }

    Print(L"[Boot] Fallback: LoadImage %s\n", mWinBootFullPath);

    Status = gBS->LoadImage(FALSE, ImageHandle, WinPath, NULL, 0, &WinImage);
    Print(L"[Boot] LoadImage returned: %r\n", Status);

    if (!EFI_ERROR(Status)) {
        Status = gBS->StartImage(WinImage, NULL, NULL);
        Print(L"[Boot] StartImage returned: %r\n", Status);
    }

    FreePool(WinPath);
    return Status;
}

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
        Print(L"[Error] EfiBootManagerGetLoadOptions returned NULL.\n");
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

        if (!Found) {
            continue;
        }

        Print(L"[Boot] Windows found: %s\n", BootOptions[Index].Description);
        Print(L"[Boot] Calling EfiBootManagerBoot...\n");

        // 试图按固件的 Boot#### 正常启动
        EfiBootManagerBoot(&BootOptions[Index]);

        // 如果能返回到这里，说明没有成功交接（失败或被固件拒绝/返回）
        Print(L"[Boot] EfiBootManagerBoot returned (handoff failed).\n");

        // 立刻 fallback：从该 BootOption 同一设备上强制启动标准 bootmgfw.efi
        StartBootmgfwFromBootOption(ImageHandle, &BootOptions[Index]);

        // 无论如何，找到一次就结束，不要继续循环也不要落到“等待按键”
        EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
        return;
    }

    Print(L"[Error] Windows boot loader not found.\n");
    EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
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

    // 如果 Windows 启动成功，通常不会再执行到这里
    Print(L"\nPress any key to exit...\n");
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
    gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    return EFI_SUCCESS;
}
