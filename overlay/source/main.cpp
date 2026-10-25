#define TESLA_INIT_IMPL

#include "exception_wrap.hpp"
#include "tune.h"
#include "gfbg.h"
#include "gui_error.hpp"
#include "gui_main.hpp"
#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"
#include "config/config.hpp"

#include <tesla.hpp>

class SysTuneOverlay final : public tsl::Overlay {
  private:
    const char *msg = nullptr;
    Result fail     = 0;

  public:
    void initServices() override {
        if (R_FAILED(pm::Initialize())) {
            this->msg  = "Failed pm::Initialize()";
            return;
        }
        Result rc = tuneInitialize();

        // not found can happen if the service isn't started
        // connection refused can happen is the service was terminated by pmshell
        if (R_VALUE(rc) == KERNELRESULT(NotFound) || R_VALUE(rc) == KERNELRESULT(ConnectionRefused)) {
            // The sysmodule isn't running, so we're about to launch it — check
            // free system RAM first. This check deliberately lives inside the
            // "service not found" path: if the module is already up and playing
            // music, re-entering the overlay must never fail on memory it
            // doesn't need to allocate.
            // Uses the same svcGetSystemInfo calls as drawMemoryWidget in gui_main.cpp:
            //   type 0, subtype 2 = total system RAM
            //   type 1, subtype 2 = used  system RAM
            {
                u64 RAM_Total_system_u = 0, RAM_Used_system_u = 0;
                svcGetSystemInfo(&RAM_Total_system_u, 0, INVALID_HANDLE, 2);
                svcGetSystemInfo(&RAM_Used_system_u,  1, INVALID_HANDLE, 2);
                const u64 freeRamBytes = RAM_Total_system_u - RAM_Used_system_u;
                constexpr u64 kMinRequired = 5872026ULL; // 5.6 MB
                if (freeRamBytes < kMinRequired) {
                    this->msg = "Not enough memory.";
                    return;
                }
            }

            u64 pid = 0;
            const NcmProgramLocation programLocation{
                .program_id = GFBG_PROGRAM_ID,   // Gatefold's background player
                .storageID  = NcmStorageId_None,
            };
            rc = pmshellInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = pmshellLaunchProgram(0, &programLocation, &pid);
                pmshellExit();
            }
            if (R_FAILED(rc) || pid == 0) {
                this->fail = rc;
                this->msg  = "  Failed to\n"
                            "launch sysmodule";
                return;
            }

            /* The sysmodule has been launched, but its IPC port isn't
             * registered yet. Poll tuneInitialize() in short intervals
             * until either it succeeds, the budget is exhausted, or a
             * non-recoverable error code comes back.
             *
             * Budget : 300 ms total
             * Poll   : 10 ms (up to 30 attempts)
             *
             * On a fast/idle system this typically returns on the 1st
             * or 2nd retry (~10-20 ms). On a busy system the budget
             * gives the service ample time to publish before we fall
             * through to the "Something went wrong" path with the
             * last rc still in hand. */
            constexpr u64 kPollIntervalNs = 10'000'000ULL;   // 10 ms
            constexpr int kMaxAttempts    = 30;              // 300 ms total

            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                svcSleepThread(kPollIntervalNs);
                rc = tuneInitialize();
                if (R_SUCCEEDED(rc))
                    break;

                /* Only NotFound / ConnectionRefused mean "service not
                 * ready yet" — anything else is a real failure and
                 * additional polling won't change the outcome. */
                if (R_VALUE(rc) != KERNELRESULT(NotFound) &&
                    R_VALUE(rc) != KERNELRESULT(ConnectionRefused))
                    break;
            }
        }

        if (R_FAILED(rc)) {
            this->msg  = "Something went wrong:";
            this->fail = rc;
            return;
        }

        if (R_FAILED(sdmc::Open())) {
            this->msg  = "Failed sdmc::Open()";
            return;
        }

        u32 api;
        if (R_FAILED(tuneGetApiVersion(&api)) || api != TUNE_API_VERSION) {
            this->msg = "   Unsupported\n"
                        "sys-tune version!";
        }
    }

    void exitServices() override {
        sdmc::Close();
        pm::Exit();
        tuneExit();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (this->msg) {
            return std::make_unique<ErrorGui>(this->msg, this->fail);
        } else {
            return std::make_unique<MainGui>();
        }
    }
};

int main(int argc, char **argv) {
    return tsl::loop<SysTuneOverlay>(argc, argv);
}