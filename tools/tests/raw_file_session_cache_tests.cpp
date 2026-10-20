#include "../../Ksword5.1/Ksword5.1/MiscDock/DiskEditor/DiskRawFileSystemInternal.h"
#include <iostream>
#include <stdexcept>
namespace ks::misc::rawfs {
    VolumeReader::VolumeReader(
        const int diskIndex,
        const unsigned long backend,
        const std::uint64_t partitionOffset,
        const std::uint64_t partitionLength,
        const std::uint32_t logicalSectorSize)
        : m_diskIndex(diskIndex),
          m_backend(backend),
          m_partitionOffset(partitionOffset),
          m_partitionLength(partitionLength),
          m_logicalSectorSize(
              logicalSectorSize == 0U ? 512U : logicalSectorSize)
    {
    }

}
int main()
{
    using ks::misc::rawfs::VolumeReader;
    unsigned checks = 0;
    const auto check = [&checks](bool value) { ++checks; if (!value) throw std::runtime_error("read session cache lifetime"); };
    struct State { int mounts = 1; int resolves = 1; int blocks = 0; };
    std::weak_ptr<State> retired;
    {
        VolumeReader reader(3, 1, 4096, 1024 * 1024, 512);
        auto state = std::make_shared<State>();
        reader.setFileSessionState(state);
        check(!reader.fileSessionState<State>());
        reader.enableFileSession();
        reader.setFileSessionState(state);
        retired = state;
        state.reset();
        check(!retired.expired());
        for (int block = 0; block < 128; ++block)
        {
            auto snapshot = reader.fileSessionState<State>();
            check(snapshot && snapshot == retired.lock());
            ++snapshot->blocks;
        }
        const auto snapshot = reader.fileSessionState<State>();
        check(snapshot->mounts == 1 && snapshot->resolves == 1 && snapshot->blocks == 128);
        check(!reader.fileSessionState<QString>());
        VolumeReader otherFile(3, 1, 4096, 1024 * 1024, 512);
        otherFile.enableFileSession();
        check(!otherFile.fileSessionState<State>());
    }
    check(retired.expired());
    std::cout << "RAW_FILE_SESSION_CACHE_CHECKS=" << checks << " FAILURES=0 GUI=0 BUSINESS_IO=0\n";
}
