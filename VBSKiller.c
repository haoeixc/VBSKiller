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

// BootCurrent 变量 GUID（UEFI 规范定义的 Global Variable）
STATIC CONST EFI_GUID gEfiGlobalVariableGuid =
    {0x8BE4DF61, 0x93CA, 0x11d2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};

STATIC CONST CHAR16 mBootCurrentName[] = L"BootCurrent";

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
 * DisableVBS：设置 VbsPolicyDisabled = 1，关闭基于虚拟化的安全
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
        Print(L"[Info] VBS already disabled, skipping.\n");
        return;
    }

    Status = gRT->SetVariable((CHAR16 *)mVbsVarName,
                             (EFI_GUID *)&gMicrosoftVendorGuid,
                             VBS_VAR_ATTR,
                             sizeof(TargetValue),
                             &TargetValue);
    if (EFI_ERROR(Status)) {
        // 变量可能以不同属性存在，先删除再创建
        gRT->SetVariable((CHAR16 *)mVbsVarName,
                         (EFI_GUID *)&gMicrosoftVendorGuid,
                         0,
                         0,
                         NULL);
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

/* ==============================================================
 * 引导修复：手动 LoadImage / StartImage 系列函数
 * 绕过 EfiBootManagerBoot() 以避免某些固件上的卡死问题
 * ============================================================== */

/*
 * ConnectAllControllersBestEffort：尽力连接所有控制器，
 * 确保存储设备及其文件系统协议可用
 */
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
        gBS->ConnectController(Handles[i], NULL, NULL, TRUE);
    }

    FreePool(Handles);
}

/*
 * SetBootCurrent：向 UEFI 全局变量 BootCurrent 写入当前引导选项编号。
 * 这是 EfiBootManagerBoot 的关键步骤，Windows Boot Manager 依赖此变量
 * 来判断引导上下文，进而决定是否读取 VbsPolicyDisabled 等策略变量。
 */
STATIC
VOID
SetBootCurrent(
    IN UINT16 BootOptionNumber
)
{
    EFI_STATUS Status;

    Status = gRT->SetVariable(
        (CHAR16 *)mBootCurrentName,
        (EFI_GUID *)&gEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(BootOptionNumber),
        &BootOptionNumber
    );

    if (EFI_ERROR(Status)) {
        Print(L"[Warn] Set BootCurrent failed: %r\n", Status);
    } else {
        Print(L"[Info] BootCurrent set to 0x%04X\n", BootOptionNumber);
    }
}

/*
 * TryLoadAndStartBootOption：使用 boot option 的完整信息启动 Windows。
 * 这模拟了 EfiBootManagerBoot 的核心逻辑，但跳过可能导致卡死的步骤。
 */
STATIC
EFI_STATUS
TryLoadAndStartBootOption(
    IN EFI_HANDLE                        ImageHandle,
    IN EFI_BOOT_MANAGER_LOAD_OPTION     *BootOption
)
{
    EFI_STATUS Status;
    EFI_HANDLE WinImage = NULL;

    Print(L"[Boot] Loading: %s\n", BootOption->Description);

    // 设置 BootCurrent，让 Windows Boot Manager 知道自己是通过正常 boot option 启动的
    SetBootCurrent(BootOption->OptionNumber);

    // 使用 boot option 的 FilePath 和 OptionalData 加载镜像
    Status = gBS->LoadImage(
        FALSE,
        ImageHandle,
        BootOption->FilePath,
        BootOption->OptionalData,
        BootOption->OptionalDataSize,
        &WinImage
    );
    Print(L"[Boot] LoadImage: %r\n", Status);

    if (!EFI_ERROR(Status)) {
        Status = gBS->StartImage(WinImage, NULL, NULL);
        Print(L"[Boot] StartImage returned: %r\n", Status);
    }

    return Status;
}

/*
 * TryStartBootmgfwOnHandle：在指定文件系统句柄上查找并启动 bootmgfw.efi
 * 这是纯盲搜兜底，不依赖任何 boot option 信息。
 */
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
    Print(L"[Boot] LoadImage (direct): %r\n", Status);

    if (!EFI_ERROR(Status)) {
        Status = gBS->StartImage(WinImage, NULL, NULL);
        Print(L"[Boot] StartImage returned: %r\n", Status);
    }

    FreePool(Dp);
    return Status;
}

/*
 * BootWindowsDirect：兜底方案——遍历所有文件系统找 bootmgfw.efi
 */
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

    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        &HandleCount,
        &Handles
    );
    if (EFI_ERROR(Status) || Handles == NULL || HandleCount == 0) {
        Print(L"[Error] No filesystems found: %r\n", Status);
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < HandleCount; i++) {
        Status = TryStartBootmgfwOnHandle(ImageHandle, Handles[i]);
        if (!EFI_ERROR(Status)) {
            FreePool(Handles);
            return Status;
        }
    }

    FreePool(Handles);
    Print(L"[Error] bootmgfw.efi not found on any filesystem.\n");
    return EFI_NOT_FOUND;
}

/*
 * BootWindows：查找 Windows Boot Manager 引导选项。
 *
 * 优先使用 boot option 的完整信息（含 OptionalData 和 OptionNumber）
 * 通过手动 LoadImage/StartImage 启动（避免 EfiBootManagerBoot 卡死），
 * 同时设置 BootCurrent 确保 Windows 能正确读取 VBS 策略变量。
 *
 * 若 boot option 方式失败，回退到盲搜 ESP。
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
    EFI_STATUS Status;

    BootOptions = EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
    if (BootOptions == NULL) {
        Print(L"[Info] No boot options, trying direct search...\n");
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
            Print(L"[Boot] Windows found: %s (Option 0x%04X)\n",
                  BootOptions[Index].Description,
                  BootOptions[Index].OptionNumber);

            // 使用 boot option 的完整上下文手动启动（修复卡死 + 保留 VBS 上下文）
            Status = TryLoadAndStartBootOption(ImageHandle, &BootOptions[Index]);

            // 如果手动启动也失败了（StartImage 返回），释放资源后走兜底
            EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
            Print(L"[Boot] Boot option start failed: %r, trying direct fallback...\n", Status);
            ConnectAllControllersBestEffort();
            BootWindowsDirect(ImageHandle);
            return;
        }
    }

    Print(L"[Error] No Windows boot option found.\n");
    EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);

    // 兜底：盲搜 ESP 上的 bootmgfw.efi
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
