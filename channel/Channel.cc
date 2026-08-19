#include "Channel.hh"

#include "channel/Apploader.hh"
#include "channel/DI.hh"
#include "channel/SRAM.hh"

#include <cube/Arena.hh>
#include <cube/Clock.hh>
#include <cube/Console.hh>
#include <cube/CubeLogger.hh>
#include <cube/DCache.hh>
#include <cube/DiscID.hh>
#include <cube/ICache.hh>
#include <cube/Platform.hh>
#include <cube/USB.hh>
#include <cube/VI.hh>
#include <cube/VirtualDI.hh>
#include <cube/ios/Resource.hh>
#include <cube/storage/EXISDStorage.hh>
#include <cube/storage/Storage.hh>
#include <cube/storage/USBStorage.hh>
#include <cube/storage/WiiSDStorage.hh>
#include <formats/Version.hh>
extern "C" {
#include <miniz/miniz.h>
}
#include <portable/Align.hh>
#include <portable/Log.hh>

extern "C" {
#include <string.h>
}

extern "C" const u8 payloadPI[];
extern "C" const size_t payloadPI_size;
extern "C" const u8 payloadEI[];
extern "C" const size_t payloadEI_size;
extern "C" const u8 payloadJI[];
extern "C" const size_t payloadJI_size;

extern "C" const u8 payloadPD[];
extern "C" const size_t payloadPD_size;
extern "C" const u8 payloadED[];
extern "C" const size_t payloadED_size;
extern "C" const u8 payloadJD[];
extern "C" const size_t payloadJD_size;

extern "C" const u8 commonArchive[];
extern "C" const size_t commonArchive_size;

extern "C" const u8 japaneseArchive[];
extern "C" const size_t japaneseArchive_size;
extern "C" const u8 englishArchive[];
extern "C" const size_t englishArchive_size;
extern "C" const u8 germanArchive[];
extern "C" const size_t germanArchive_size;
extern "C" const u8 frenchArchive[];
extern "C" const size_t frenchArchive_size;
extern "C" const u8 spanishArchive[];
extern "C" const size_t spanishArchive_size;
extern "C" const u8 italianArchive[];
extern "C" const size_t italianArchive_size;

extern "C" u32 consoleType;
extern "C" u32 arenaLo;
extern "C" u32 arenaHi;
extern "C" u32 iosVersion;

extern "C" volatile u32 aicr;

extern "C" volatile u32 armirqmask;
extern "C" volatile u32 aipprot;

#pragma section RWX "hwtest"

typedef u32 (*MachineFunc)(u32 r3, u32 r4);
#define CallMachineFunc(func) ((MachineFunc)(uintptr_t)(func))

// Configures a GQR
static u32 HardwareTest_MakeGQR(u32 loadScale, u32 loadType, u32 storeScale, u32 storeType) {
    // Scales must fit in 6 bits
    if (loadScale >= (1 << 6) || storeScale >= (1 << 6)) {
        return 0xffffffff;
    }

    // Types must fit in 3 bits
    if (loadType >= (1 << 3) || storeType >= (1 << 3)) {
        return 0xffffffff;
    }

    return (loadScale << 24) | (loadType << 16) | (storeScale << 8) | (storeType << 0);
}

static u32 HardwareTest_MakeMTGQR(u32 gqr, u32 gpr) {
    // GQR must fit in 3 bits
    if (gqr >= (1 << 3)) {
        return 0xffffffff;
    }

    // GPR must fit in 5 bits
    if (gpr >= (1 << 5)) {
        return 0xffffffff;
    }

    return (gpr << 21) | (gqr << 16) | 0x7c10e3a6;
}

// Initializes a configured GPR with r3
// void HardwareTest_SetGQR(u32 gqr);
__declspec(section "hwtest") static u32 HardwareTest_SetGQR[] = {
        0x60000000, // mtspr GQR?, r3;
        0x4e800020, // blr;
};

__declspec(section "hwtest") static const u32 HardwareTest_CalcGQR[] = {
        0xe0038000, // psq_l f0, 0x0(r3), 1, gqr0;
        0xf0048000, // psq_st f0, 0x0(r4), 1, gqr0;
        0xe0239008, // psq_l f1, 0x8(r3), 1, gqr1;
        0xf0249008, // psq_st f1, 0x8(r4), 1, gqr1;
        0xe043a010, // psq_l f2, 0x10(r3), 1, gqr2;
        0xf044a010, // psq_st f2, 0x10(r4), 1, gqr2;
        0xe063b018, // psq_l f3, 0x18(r3), 1, gqr3;
        0xf064b018, // psq_st f3, 0x18(r4), 1, gqr3;
        0xe083c020, // psq_l f4, 0x20(r3), 1, gqr4;
        0xf084c020, // psq_st f4, 0x20(r4), 1, gqr4;
        0xe0a3d028, // psq_l f5, 0x28(r3), 1, gqr5;
        0xf0a4d028, // psq_st f5, 0x28(r4), 1, gqr5;
        0xe0c3e030, // psq_l f6, 0x30(r3), 1, gqr6;
        0xf0c4e030, // psq_st f6, 0x30(r4), 1, gqr6;
        0xe0e3f038, // psq_l f7, 0x38(r3), 1, gqr7;
        0xf0e4f038, // psq_st f7, 0x38(r4), 1, gqr7;
        0x4e800020, // blr;
};

#define T0S0 0
#define T1S0 1
#define T2S0 2
#define T3S0 3
#define T0S2 4
#define T1S2 5
#define T2S2 6
#define T3S2 7

static void HardwareTest_(void) {
    u32 gqrs[8] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
            0xffffffff, 0xffffffff};
    // We use gotos, so we need to declare these up here
    u32 val;
    bool success;
    u32 inputBuffer[8];
    u32 outputBuffer[8];

    for (u32 type = 0; type < 4; ++type) {
        u32 gqrScale0 = HardwareTest_MakeGQR(0, type, 0, type);
        u32 gqrScale2 = HardwareTest_MakeGQR(2, type, 2, type);

        if (gqrScale0 == 0xffffffff) {
            ERROR("Invalid GQR%d!", type);
            goto HardwareTest_Finish;
        }

        if (gqrScale2 == 0xffffffff) {
            ERROR("Invalid GQR%d!", type + 4);
            goto HardwareTest_Finish;
        }

        gqrs[type] = gqrScale0;
        gqrs[type + 4] = gqrScale2;
    }

    for (u32 gqr = 0; gqr < 8; ++gqr) {
        u32 mtgqr = HardwareTest_MakeMTGQR(gqr, 3);
        if (mtgqr == 0xffffffff) {
            ERROR("Invalid MTGQR%d command!", gqr);
            goto HardwareTest_Finish;
        }

        HardwareTest_SetGQR[0] = mtgqr;
        ICache::Invalidate(HardwareTest_SetGQR, sizeof(u32));
        DCache::Flush(HardwareTest_SetGQR, sizeof(u32));
        CallMachineFunc(HardwareTest_SetGQR)(gqrs[gqr], 0);
    }

    INFO("Initialized GQRs.");

    success = true;

    val = 0;
    do {
        for (u32 i = 0; i < 8; ++i) {
            inputBuffer[i] = val;
            outputBuffer[i] = 0;
        }

        CallMachineFunc(HardwareTest_CalcGQR)(reinterpret_cast<uintptr_t>(&inputBuffer),
                reinterpret_cast<uintptr_t>(&outputBuffer));

        for (u32 i = 1; i < 4; ++i) {
            if (outputBuffer[i] != outputBuffer[0]) {
                WARN("T%dS0 did not match T0S0 for %08x! (Expected 0x%08x, got 0x%08x)", i, val,
                        outputBuffer[0], outputBuffer[i]);
                success = false;
            }

            if (outputBuffer[i + 4] != outputBuffer[4]) {
                WARN("T%dS2 did not match T0S2 for 0x%08x! (Expected 0x%08x, got 0x%08x)", i, val,
                        outputBuffer[4], outputBuffer[i + 4]);
                success = false;
            }
        }
    } while (val++ != 0xffffffff);

    if (success) {
        INFO("Tests passed!");
    } else {
        ERROR("Tests failed!");
    }

HardwareTest_Finish:
    volatile bool b = false;
    do { } while (!b); }

Channel::PayloadEntryFunc Channel::Run(Context *context) {
    VI::Init();
    Console::Init(VI::Instance());
    CubeLogger::Init();
    INFO("Double Dash Deluxe Channel [%s]", Version);

    if (!Platform::IsGameCube()) {
        if (iosVersion >> 16 != 58 && iosVersion >> 16 != 59) {
            ERROR("Double Dash Deluxe needs IOS58 (or IOS59).");
            ERROR("Please perform a Wii System Update or use the IOS58 Installer to install");
            ERROR("IOS58.");
            return nullptr;
        }

        context->iosVersion = iosVersion;
        Platform::s_iosVersion = iosVersion;

        IOS::Resource::Init();
    }
    Clock::Init();

    if (Platform::IsDolphin()) {
        DolphinVersion dolphinVersion;
        DolphinVersion minimumDolphinVersion = {5, 0, 20189};
        if (!Platform::GetDolphinVersion(dolphinVersion) ||
                dolphinVersion < minimumDolphinVersion) {
            WARN("Using DSP HLE will result in broken sound on this Dolphin version!");
            WARN("Please update to Dolphin 5.0-20189 or later.");
            Clock::WaitMilliseconds(1000);
        }
    }

    if (!Platform::IsGameCube()) {
        // Use compat MMIO ranges for DI/SI/EXI/AI
        aipprot &= ~(1 << 0);

        // Prevent Starlet from receiving DI interrupts
        armirqmask &= ~(1 << 18);
    }

    // Reset the DSP: libogc apps like the HBC cannot initialize it properly, but the SDK can.
    aicr = 0;

    // HIJACK: This does not return
    HardwareTest_();

    Storage::Init();
    if (!Platform::IsGameCube()) {
        USBStorage::Init();
        USB::Init();
        WiiSDStorage::Init();
    }
    VirtualDI::Init();

    RunApploader(context);

    void *payloadIDst;
    const void *payloadISrc;
    size_t payloadISrcSize;
    const void *payloadDSrc;
    size_t payloadDSrcSize;
    switch (DiscID::Get().gameID[3]) {
    case 'P':
        payloadIDst = reinterpret_cast<void *>(0x802d84a0);
        payloadISrc = &payloadPI;
        payloadISrcSize = payloadPI_size;
        payloadDSrc = &payloadPD;
        payloadDSrcSize = payloadPD_size;
        break;
    case 'E':
        payloadIDst = reinterpret_cast<void *>(0x802d8520);
        payloadISrc = &payloadEI;
        payloadISrcSize = payloadEI_size;
        payloadDSrc = &payloadED;
        payloadDSrcSize = payloadED_size;
        break;
    case 'J':
        payloadIDst = reinterpret_cast<void *>(0x802d8540);
        payloadISrc = &payloadJI;
        payloadISrcSize = payloadJI_size;
        payloadDSrc = &payloadJD;
        payloadDSrcSize = payloadJD_size;
        break;
    default:
        ERROR("Region detection failed!");
        return nullptr;
    }

    INFO("Decompressing payload instructions...");
    size_t payloadIDstSize = tinfl_decompress_mem_to_mem(payloadIDst,
            0x80360000 - reinterpret_cast<uintptr_t>(payloadIDst), payloadISrc, payloadISrcSize, 0);
    DCache::Flush(payloadIDst, payloadIDstSize);
    ICache::Invalidate(payloadIDst, payloadIDstSize);
    INFO("Decompressed payload instructions.");

    INFO("Decompressing payload data...");
    uintptr_t payloadDDstAddress = 0x80800000;
    void *payloadDDst = reinterpret_cast<void *>(payloadDDstAddress);
    size_t payloadDDstSize =
            tinfl_decompress_mem_to_mem(payloadDDst, 128 * 1024, payloadDSrc, payloadDSrcSize, 0);
    INFO("Decompressed payload data.");

    INFO("Copying common archive...");
    context->commonArchive = MEM1Arena::Instance()->alloc(commonArchive_size, -0x20);
    context->commonArchiveSize = commonArchive_size;
    memcpy(context->commonArchive, &commonArchive, commonArchive_size);
    INFO("Copied common archive.");

    INFO("Copying localized archive...");
    const u8 *localizedArchive;
    size_t localizedArchiveSize;
    GetLocalizedArchive(localizedArchive, localizedArchiveSize);
    context->localizedArchive = MEM1Arena::Instance()->alloc(localizedArchiveSize, -0x20);
    context->localizedArchiveSize = localizedArchiveSize;
    memcpy(context->localizedArchive, localizedArchive, localizedArchiveSize);
    INFO("Copied localized archive.");

    if (Platform::IsDolphin()) {
        // Enable OSReport over EXI
        consoleType |= 0x10000000;
    }
    arenaLo = payloadDDstAddress + payloadDDstSize;
    arenaHi = reinterpret_cast<uintptr_t>(MEM1Arena::Instance()->alloc(0x0, -0x4));

    INFO("Copying low memory...");
    uintptr_t lowMemoryDstAddress = 0x80400000;
    void *lowMemoryDst = reinterpret_cast<void *>(lowMemoryDstAddress);
    void *lowMemorySrc = reinterpret_cast<void *>(0x80000000);
    size_t lowMemorySize = 0x56c0;
    memcpy(lowMemoryDst, lowMemorySrc, lowMemorySize);
    INFO("Copied low memory.");

    INFO("Starting payload...");
    PayloadEntryFunc payloadEntry = reinterpret_cast<PayloadEntryFunc>(payloadIDst);
    return payloadEntry;
}

void Channel::RunApploader(Context *context) {
    while (true) {
        if (RunApploaderFromVirtualDI()) {
            context->hasVirtualDI = true;
            return;
        }
        if (DI::ReadDiscID()) {
            const char *gameID = DiscID::Get().gameID;
            if (DiscID::IsValid()) {
                INFO("Mario Kart: Double Dash!! disc found (game id %.4s).", gameID);
                if (Apploader::Run(DI::Read)) {
                    context->hasVirtualDI = false;
                    return;
                }
            } else {
                ERROR("This is not Mario Kart: Double Dash!! (game id %.4s).", gameID);
            }
            while (DI::ReadDiscID()) {
                Clock::WaitMilliseconds(100);
                if (RunApploaderFromVirtualDI()) {
                    context->hasVirtualDI = true;
                    return;
                }
            }
        } else {
            if (DI::IsInserted()) {
                INFO("Resetting disc interface...");
                DI::Reset();
                INFO("Reset disc interface.");
            } else {
                if (Platform::IsDolphin()) {
                    WARN("Insert the Mario Kart: Double Dash!! disc by right-clicking the game");
                    WARN("in the game list and selecting \"Change Disc\".");
                    WARN("To avoid this in the future, select \"Set as Default ISO\" as well.");
                } else {
                    WARN("Please insert a Mario Kart: Double Dash!! disc.");
                }
                while (!DI::IsInserted()) {
                    Clock::WaitMilliseconds(100);
                    if (RunApploaderFromVirtualDI()) {
                        context->hasVirtualDI = true;
                        return;
                    }
                }
            }
        }
    }
}

bool Channel::RunApploaderFromVirtualDI() {
    bool enableEXISD = true;
    bool enableUSB = !Platform::IsGameCube() && !Platform::IsDolphin();
    bool enableWiiSD = !Platform::IsGameCube();
    return RunApploaderFromVirtualDI(enableEXISD, enableUSB, enableWiiSD);
}

bool Channel::RunApploaderFromVirtualDI(bool enableEXISD, bool enableUSB, bool enableWiiSD) {
    if (enableEXISD) {
        EXISDStorage exiSDStorage0(0);
        EXISDStorage exiSDStorage1(1);
        EXISDStorage exiSDStorage2(2);
        EXISDStorage exiSDStorage3(3);

        return RunApploaderFromVirtualDI(false, enableUSB, enableWiiSD);
    }

    if (enableUSB) {
        USB::Handle usbHandle;

        return RunApploaderFromVirtualDI(enableEXISD, false, enableWiiSD);
    }

    if (enableWiiSD) {
        WiiSDStorage wiiSDStorage;

        return RunApploaderFromVirtualDI(enableEXISD, enableUSB, false);
    }

    if (!VirtualDI::Mount()) {
        return false;
    }

    if (!Apploader::Run(VirtualDI::Read)) {
        return false;
    }

    return true;
}

void Channel::GetLocalizedArchive(const u8 *&archive, size_t &archiveSize) {
    char region = DiscID::Get().gameID[3];
    if (region == 'J') {
        archive = japaneseArchive;
        archiveSize = japaneseArchive_size;
        return;
    }

    archive = englishArchive;
    archiveSize = englishArchive_size;

    if (region != 'P') {
        return;
    }

    SRAM sram;
    u8 language;
    if (!sram.getLanguage(language)) {
        return;
    }
    switch (language) {
    case SRAM::Language::English:
    case SRAM::Language::German:
    case SRAM::Language::French:
    case SRAM::Language::Spanish:
    case SRAM::Language::Italian:
        break;
    default:
        return;
    }

    const u8 *archives[] = {
            englishArchive,
            germanArchive,
            frenchArchive,
            spanishArchive,
            italianArchive,
    };
    size_t archiveSizes[] = {
            englishArchive_size,
            germanArchive_size,
            frenchArchive_size,
            spanishArchive_size,
            italianArchive_size,
    };
    archive = archives[language];
    archiveSize = archiveSizes[language];
}
