#pragma once

#include <cube/Context.hh>

class Channel {
public:
    typedef void (*PayloadEntryFunc)(Context *context);

    static PayloadEntryFunc Run(Context *context);

private:
    class ApploaderState {
    public:
        enum {
            Initialize = 0,
            TestInserted = 1,
            TestUninserted = 2,
            WaitEjection = 3,
            WaitInsertion = 4,
            Done = 5,
        };

    private:
        ApploaderState();
    };

    Channel();

    static void RunApploader(Context *context);
    static bool RunApploaderFromVirtualDI();
    static bool RunApploaderFromVirtualDI(bool enableEXISD, bool enableUSB, bool enableWiiSD);
    static void GetLocalizedArchive(const u8 *&archive, size_t &archiveSize);
};
