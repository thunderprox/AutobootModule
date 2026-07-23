#include "BootUtils.h"
#include "ACTAccountInfo.h"
#include "MenuUtils.h"
#include "logger.h"
#include <codecvt>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/thread.h>
#include <cstdarg>
#include <cstdio>
#include <coreinit/time.h>
#include <filesystem>
#include <locale>
#include <malloc.h>
#include <memory>
#include <mocha/mocha.h>
#include <nn/act.h>
#include <nn/cmpt/cmpt.h>
#include <padscore/kpad.h>
#include <sndcore2/core.h>
#include <string>
#include <sysapp/launch.h>
#include <sysapp/title.h>
#include <vector>
#include <vpad/input.h>

void handleAccountSelection();

void bootWiiUMenu() {
    nn::act::Initialize();
    nn::act::SlotNo slot        = nn::act::GetSlotNo();
    nn::act::SlotNo defaultSlot = nn::act::GetDefaultAccount();
    nn::act::Finalize();

    if (defaultSlot) { //normal menu boot
        SYSLaunchMenu();
    } else { //show mii select
        _SYSLaunchMenuWithCheckingAccount(slot);
    }
}

void bootHomebrewLauncher() {
    handleAccountSelection();

    uint64_t titleId = _SYSGetSystemApplicationTitleId(SYSTEM_APP_ID_MII_MAKER);
    _SYSLaunchTitleWithStdArgsInNoSplash(titleId, nullptr);
}

void handleAccountSelection() {
    nn::act::Initialize();
    nn::act::SlotNo defaultSlot = nn::act::GetDefaultAccount();

    if (!defaultSlot) { // No default account is set.
        std::vector<std::shared_ptr<AccountInfo>> accountInfoList;
        for (int32_t i = 0; i < 13; i++) {
            if (!nn::act::IsSlotOccupied(i)) {
                continue;
            }
            char16_t nameOut[nn::act::MiiNameSize];
            std::shared_ptr<AccountInfo> accountInfo = std::make_shared<AccountInfo>();
            accountInfo->slot                        = i;
            auto result                              = nn::act::GetMiiNameEx(reinterpret_cast<int16_t *>(nameOut), i);
            if (result.IsSuccess()) {
                accountInfo->name = std::filesystem::path(nameOut).string();
            } else {
                accountInfo->name = "[UNKNOWN]";
            }
            accountInfo->isNetworkAccount = nn::act::IsNetworkAccountEx(i);
            if (accountInfo->isNetworkAccount) {
                nn::act::GetAccountIdEx(accountInfo->accountId, i);
            }

            uint32_t imageSize = 0;
            result             = nn::act::GetMiiImageEx(&imageSize, accountInfo->miiImageBuffer, sizeof(accountInfo->miiImageBuffer), 0, i);
            if (result.IsSuccess()) {
                accountInfo->miiImageSize = imageSize;
            }
            accountInfoList.push_back(accountInfo);
        }

        if (accountInfoList.size() > 0) {
            if (!AXIsInit()) {
                AXInit();
            }
            auto slot = handleAccountSelectScreen(accountInfoList);

            DEBUG_FUNCTION_LINE("Load slot %d", slot);
            nn::act::LoadConsoleAccount(slot, 0, nullptr, false);
        }
    }
    nn::act::Finalize();
}

#ifdef DEBUG
// Appends diagnostics to a log file on the sd card which can be read on a PC,
// since the console may freeze or reboot into vWii mode during the launch.
static void diagLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void diagLog(const char *fmt, ...) {
    FILE *f = fopen("fs:/vol/external01/wiiu/autoboot_diag.log", "a");
    if (!f) {
        return;
    }
    fprintf(f, "[%llu ms] ", (unsigned long long) OSTicksToMilliseconds(OSGetSystemTime()));
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputs("\n", f);
    fclose(f);
}
#else
static inline void diagLog(const char *, ...) {}
#endif

static bool isGamePadAttached() {
    // A connected GamePad streams input samples continuously, so only a
    // successful read proves it's attached. Retry on VPAD_READ_NO_SAMPLES
    // like InputUtils::getControllerInput does, since it can also show up
    // between two samples of a connected GamePad.
    VPADReadError error = VPAD_READ_UNINITIALIZED;
    int maxAttempts     = 100;
    do {
        VPADStatus status = {};
        if (VPADRead(VPAD_CHAN_0, &status, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
            DEBUG_FUNCTION_LINE("GamePad is attached");
            diagLog("GamePad is attached (attempts left %d)", maxAttempts);
            return true;
        }
        OSSleepTicks(OSMillisecondsToTicks(1));
    } while (--maxAttempts > 0 && error == VPAD_READ_NO_SAMPLES);

    DEBUG_FUNCTION_LINE("GamePad is not attached (VPADRead error %d)", error);
    diagLog("GamePad is not attached (VPADRead error %d, attempts left %d)", error, maxAttempts);
    return false;
}

static void launchvWiiTitle(uint64_t titleId) {
    // we need to init kpad for cmpt
    KPADInit();

    diagLog("--- launching vWii title %016llx", (unsigned long long) titleId);

    // Try to find a screen type that works
    int32_t rc;
    if (!isGamePadAttached()) {
        // CMPTCheckScreenState only checks the video output configuration, so
        // it accepts CMPT_SCREEN_TYPE_BOTH even without a GamePad attached,
        // which freezes the console on launch. Force TV-only in that case.
        DEBUG_FUNCTION_LINE("No GamePad attached, using CMPT_SCREEN_TYPE_TV");
        rc = CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_TV);
        diagLog("CMPTAcctSetScreenType(TV) = %d", rc);
    } else {
        DEBUG_FUNCTION_LINE("Using CMPT_SCREEN_TYPE_BOTH");
        rc = CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_BOTH);
        diagLog("CMPTAcctSetScreenType(BOTH) = %d", rc);
        if ((rc = CMPTCheckScreenState()) < 0) {
            diagLog("CMPTCheckScreenState() = %d, falling back to DRC", rc);
            DEBUG_FUNCTION_LINE("Falling back to CMPT_SCREEN_TYPE_DRC");
            rc = CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_DRC);
            diagLog("CMPTAcctSetScreenType(DRC) = %d", rc);
            if ((rc = CMPTCheckScreenState()) < 0) {
                diagLog("CMPTCheckScreenState() = %d, falling back to TV", rc);
                DEBUG_FUNCTION_LINE("Falling back to CMPT_SCREEN_TYPE_TV");
                rc = CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_TV);
                diagLog("CMPTAcctSetScreenType(TV) = %d", rc);
            }
        }
    }

    uint32_t dataSize = 0;
    rc                = CMPTGetDataSize(&dataSize);
    diagLog("CMPTGetDataSize() = %d, dataSize = %u", rc, dataSize);

    void *dataBuffer = memalign(0x40, dataSize);

    if (titleId == 0) {
        diagLog("calling CMPTLaunchMenu");
        rc = CMPTLaunchMenu(dataBuffer, dataSize);
    } else {
        diagLog("calling CMPTLaunchTitle");
        rc = CMPTLaunchTitle(dataBuffer, dataSize, titleId);
    }
    diagLog("CMPTLaunch returned %d", rc);

    free(dataBuffer);
}

void bootvWiiMenu() {
    launchvWiiTitle(0);
}

uint64_t getVWiiHBLTitleId() {
    // fall back to booting the vWii system menu if anything fails
    uint64_t titleId = 0;

    FSAInit();
    auto client = FSAAddClient(nullptr);
    if (client > 0) {
        if (Mocha_UnlockFSClientEx(client) == MOCHA_RESULT_SUCCESS) {
            // mount the slccmpt
            if (FSAMount(client, "/dev/slccmpt01", "/vol/storage_abm_slccmpt01", FSA_MOUNT_FLAG_GLOBAL_MOUNT, nullptr, 0) >= 0) {
                FSStat stat;

                // test if the OHBC or HBC is installed
                if (FSAGetStat(client, "/vol/storage_abm_slccmpt01/title/00010001/4f484243/content/00000000.app", &stat) >= 0) {
                    titleId = 0x000100014F484243L; // 'OHBC'
                } else if (FSAGetStat(client, "/vol/storage_abm_slccmpt01/title/00010001/4c554c5a/content/00000000.app", &stat) >= 0) {
                    titleId = 0x000100014C554C5AL; // 'LULZ'
                } else {
                    DEBUG_FUNCTION_LINE("Cannot find HBC");
                }
                FSAUnmount(client, "/vol/storage_abm_slccmpt01", FSA_UNMOUNT_FLAG_FORCE);
            } else {
                DEBUG_FUNCTION_LINE_ERR("Failed to mount slccmpt01");
            }
        } else {
            DEBUG_FUNCTION_LINE_ERR("Failed to unlock FSClient");
        }
        FSADelClient(client);
    } else {
        DEBUG_FUNCTION_LINE_ERR("Failed to add FSAClient");
    }
    return titleId;
}

void bootHomebrewChannel() {
    uint64_t titleId = getVWiiHBLTitleId();
    DEBUG_FUNCTION_LINE("Launching vWii title %016llx", titleId);
    launchvWiiTitle(titleId);
}
