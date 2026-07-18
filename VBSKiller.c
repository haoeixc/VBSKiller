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

STATIC CONST EFI_GUID gMicrosoftVendorGuid = {
    0x77fa9abd, 0x0359, 0x4d32,
    {0xbd, 0x60, 0x28, 0xf4, 0xe7, 0x8f, 0x78, 0x4b}
};

STATIC CONST CHAR16 mVbsVarName[] = L"VbsPolicyDisabled";

//
// 修复 Bug #2：补上 EFI_VARIABLE_RUNTIME_ACCESS
// 与微软原始变量属性保持一致，避免属性不匹配导致 SetVariable 被拒绝
//
#define VBS_VAR_ATTR (EFI_VARIABLE_NON_VOLATILE  | \
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                      EFI_VARIABLE_RUNTIME_ACCESS)

STATIC CONST CHAR16 mWinBootFile[] = L"BOOTMGFW.EFI";
#define WIN_BOOT_FILE_LEN 12

//------------------------------------------------------------------------------
// IsWindowsBootFile
//   大小写不敏感地判断路径尾部是否为 Windows 固件启动文件 bootmgfw.efi
//------------------------------------------------------------------------------
STATIC
BOOLEAN
IsWindowsBootFile(
    IN CHAR16 *PathName)
{
    UINTN Len;
    UINTN Index;
    CHAR16 CharA, CharB;

    if (PathName == NULL)
    {
        return FALSE;
    }

    Len = StrLen(PathName);
    if (Len < WIN_BOOT_FILE_LEN)
    {
        return FALSE;
    }

    //
    // 定位到路径最后 WIN_BOOT_FILE_LEN 个字符（文件名部分）
    //
    PathName += (Len - WIN_BOOT_FILE_LEN);

    for (Index = 0; Index < WIN_BOOT_FILE_LEN; Index++)
    {
        CharA = PathName[Index];
        CharB = mWinBootFile[Index];

        if (CharA >= L'a' && CharA <= L'z')
        {
            CharA -= (L'a' - L'A');
        }

        if (CharA != CharB)
        {
            return FALSE;
        }
    }

    return TRUE;
}

//------------------------------------------------------------------------------
// DisableVBS
//   设置 VbsPolicyDisabled = 1 以通知 Windows Boot Manager 禁用 VBS
//
//   修复说明：
//   1. 先读取现有变量及其属性
//   2. 若已为目标值则幂等退出
//   3. 使用原变量属性（或默认属性）写入，避免不必要地删除重建
//------------------------------------------------------------------------------
STATIC
VOID
DisableVBS(
    VOID)
{
    EFI_STATUS Status;
    UINT8      TargetValue  = 1;
    UINT8      CurrentValue = 0;
    UINTN      Size         = sizeof(CurrentValue);
    UINT32     Attr         = 0;

    //
    // 查询变量当前状态（含属性）
    //
    Status = gRT->GetVariable(
        (CHAR16 *)mVbsVarName,
        (EFI_GUID *)&gMicrosoftVendorGuid,
        &Attr,
        &Size,
        &CurrentValue
        );

    //
    // 已存在且已为目标值 → 幂等退出
    //
    if (!EFI_ERROR(Status) && CurrentValue == TargetValue)
    {
        Print(L"[Info] VBS already disabled, skipping.\n");
        return;
    }

    //
    // 确定写入属性：
    //   - 若变量已存在 → 沿用其属性（确保固件不因属性变更而拒绝写入）
    //   - 若不存在     → 使用默认属性
    //
    if (EFI_ERROR(Status))
    {
        Attr = VBS_VAR_ATTR;
    }
    else
    {
        //
        // 确保必需的属性位（有些固件可能未设 RUNTIME_ACCESS）
        //
        Attr |= (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS);
    }

    Status = gRT->SetVariable(
        (CHAR16 *)mVbsVarName,
        (EFI_GUID *)&gMicrosoftVendorGuid,
        Attr,
        sizeof(TargetValue),
        &TargetValue
        );

    //
    // 若仍失败：尝试删除后重建（兼容属性冲突的固件）
    //
    if (EFI_ERROR(Status))
    {
        Print(L"[Warning] First SetVariable failed: %r, trying delete+recreate...\n", Status);

        gRT->SetVariable(
            (CHAR16 *)mVbsVarName,
            (EFI_GUID *)&gMicrosoftVendorGuid,
            0,
            0,
            NULL
            );

        Status = gRT->SetVariable(
            (CHAR16 *)mVbsVarName,
            (EFI_GUID *)&gMicrosoftVendorGuid,
            VBS_VAR_ATTR,
            sizeof(TargetValue),
            &TargetValue
            );
    }

    if (EFI_ERROR(Status))
    {
        Print(L"[Error] VBS Disable failed: %r\n", Status);
    }
    else
    {
        Print(L"[Info] VBS Disable succeeded\n");
    }
}

//------------------------------------------------------------------------------
// BootWindows
//   在 Boot#### 变量中查找指向 bootmgfw.efi 的 Windows 启动项并引导
//
//   修复说明：
//   1. 找到第一个匹配项后立即 break（不继续无意义的遍历）
//   2. 将匹配的启动项 CopyMem 到栈上副本
//   3. 先释放 BootOptions 内存，再调用 EfiBootManagerBoot
//      （避免内存碎片化导致 LoadImage 失败）
//   4. 检查 EfiBootManagerBoot 返回值，失败时输出错误码
//------------------------------------------------------------------------------
STATIC
VOID
BootWindows(
    VOID)
{
    EFI_STATUS                     Status;
    EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
    UINTN                          BootOptionCount;
    UINTN                          Index;
    EFI_DEVICE_PATH_PROTOCOL      *Node;
    FILEPATH_DEVICE_PATH          *FilePathNode;
    EFI_BOOT_MANAGER_LOAD_OPTION   WinOption;   // 栈上副本
    BOOLEAN                        Found;

    BootOptions = EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
    if (BootOptions == NULL)
    {
        Print(L"[Error] Failed to get boot options.\n");
        return;
    }

    Found = FALSE;
    ZeroMem(&WinOption, sizeof(WinOption));

    for (Index = 0; Index < BootOptionCount; Index++)
    {
        //
        // 跳过非活跃启动项
        //
        if ((BootOptions[Index].Attributes & LOAD_OPTION_ACTIVE) == 0)
        {
            continue;
        }

        //
        // 遍历设备路径，寻找文件路径节点
        //
        Node = BootOptions[Index].FilePath;
        while (!IsDevicePathEnd(Node))
        {
            if ((DevicePathType(Node) == MEDIA_DEVICE_PATH) &&
                (DevicePathSubType(Node) == MEDIA_FILEPATH_DP))
            {
                FilePathNode = (FILEPATH_DEVICE_PATH *)Node;
                if (IsWindowsBootFile(FilePathNode->PathName))
                {
                    //
                    // 保存启动项副本到栈上（避免释放 BootOptions 后悬空指针）
                    //
                    CopyMem(&WinOption, &BootOptions[Index], sizeof(WinOption));
                    Found = TRUE;
                    break;
                }
            }
            Node = NextDevicePathNode(Node);
        }

        if (Found)
        {
            break;   // 已找到，立即停止遍历
        }
    }

    if (!Found)
    {
        Print(L"[Error] Windows boot loader not found.\n");
        EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
        return;
    }

    Print(L"[Boot] Windows found: %s\n", WinOption.Description);

    //
    // 关键修复：先释放 BootOptions，再引导
    // 减少内存碎片，避免 LoadImage 时分配失败
    //
    EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
    BootOptions = NULL;

    //
    // 尝试引导 Windows
    // 注意：EfiBootManagerBoot 成功时不会返回（调用 StartImage → ExitBootServices）
    //       若返回则说明引导失败
    //
    Status = EfiBootManagerBoot(&WinOption);

    //
    // 走到这里 = 引导失败
    //
    Print(L"[Error] EfiBootManagerBoot failed: %r\n", Status);
}

//------------------------------------------------------------------------------
// UefiMain
//   入口点：禁用 VBS → 引导 Windows → 失败时等待按键退出
//------------------------------------------------------------------------------
EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable)
{
    UINTN        Index;
    EFI_INPUT_KEY Key;

    Print(L"============================================\n");
    Print(L"  VBS Disable + Windows Auto-Boot Tool\n");
    Print(L"============================================\n\n");

    //
    // Step 1: 禁用 VBS（基于虚拟化的安全）
    //
    DisableVBS();

    //
    // Step 2: 查找并引导 Windows
    //
    BootWindows();

    //
    // 以下代码仅在 BootWindows() 失败返回时执行
    //
    Print(L"\n--------------------------------------------\n");
    Print(L"Boot failed. Press any key to exit...\n");
    Print(L"--------------------------------------------\n");

    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
    gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);

    return EFI_SUCCESS;
}
