// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>
#include <cryptopp/hex.h>
#include <fmt/ranges.h>
#include <zstd.h>
#include "common/archives.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/swap.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "core/savestate_data.h"
#include "network/network.h"

namespace Core {

#pragma pack(push, 1)
struct CSTHeader {
    std::array<u8, 4> filetype;    /// Unique Identifier to check the file type (always "CST"0x1B)
    u64_le program_id;             /// ID of the ROM being executed. Also called title_id
    std::array<u8, 20> revision;   /// Git hash of the revision this savestate was created with
    u64_le time;                   /// The time when this save state was created
    std::array<u8, 20> build_name; /// The build name (Canary/Nightly) with the version number
    u32_le zero = 0;               /// Should be zero, just in case.

    std::array<u8, 192> reserved{}; /// Make heading 256 bytes so it has consistent size
};
static_assert(sizeof(CSTHeader) == 256, "CSTHeader should be 256 bytes");
#pragma pack(pop)

constexpr std::array<u8, 4> header_magic_bytes{{'C', 'S', 'T', 0x1B}};

std::string GetSaveStatePath(u64 program_id, u64 movie_id, u32 slot) {
    if (movie_id) {
        return fmt::format("{}{:016X}.movie{:016X}.{:02d}.cst",
                           FileUtil::GetUserPath(FileUtil::UserPath::StatesDir), program_id,
                           movie_id, slot);
    } else {
        return fmt::format("{}{:016X}.{:02d}.cst",
                           FileUtil::GetUserPath(FileUtil::UserPath::StatesDir), program_id, slot);
    }
}

static bool ValidateSaveState(const CSTHeader& header, SaveStateInfo& info, u64 program_id,
                              u64 movie_id) {
    const auto path = GetSaveStatePath(program_id, movie_id, info.slot);
    if (header.filetype != header_magic_bytes) {
        LOG_WARNING(Core, "Invalid save state file {}", path);
        return false;
    }
    info.time = header.time;

    if (header.program_id != program_id) {
        LOG_WARNING(Core, "Save state file isn't for the current game {}", path);
        return false;
    }
    const std::string revision = fmt::format("{:02x}", fmt::join(header.revision, ""));
    const std::string build_name =
        header.zero == 0 ? reinterpret_cast<const char*>(header.build_name.data()) : "";

    if (revision == Common::g_scm_rev) {
        info.status = SaveStateInfo::ValidationStatus::OK;
        return true;
    }

    // Different revision than this build. Determine a display name for the log, then REJECT.
    // xappify fork: Delta hosts a single 3DS core id shared by the legacy prebuilt Citra and this
    // self-built Cytrus, so a snapshot from another build/core reaches here with an incompatible
    // serialized System layout. Deserializing it corrupts state (and a pre-migration prebuilt-Citra
    // save previously aborted at the `revision == g_scm_rev` compare when g_scm_rev was null).
    // Refuse before reading the body: System::LoadState throws "Invalid savestate", RunLoop catches
    // it (ErrorSavestate), and the freshly-booted title keeps running. Cytrus's own saves match
    // above (stable g_scm_rev) and are unaffected. Upstream instead force-accepted on iOS.
    if (!build_name.empty()) {
        info.build_name = build_name;
    } else if (hash_to_version.find(revision) != hash_to_version.end()) {
        info.build_name = hash_to_version.at(revision);
    }
    LOG_WARNING(Core, "Save state file {} is from a different revision {} (this build: {}); rejecting",
                path, revision, Common::g_scm_rev);
    info.status = SaveStateInfo::ValidationStatus::RevisionDismatch;
    return false;
}

#if TARGET_OS_IOS
std::vector<std::pair<SaveStateInfo, std::optional<std::string>>> ListSaveStates(u64 program_id,
                                                                                 u64 movie_id) {
    std::vector<std::pair<SaveStateInfo, std::optional<std::string>>> result;
    result.reserve(SaveStateSlotCount);
#else
std::vector<SaveStateInfo> ListSaveStates(u64 program_id, u64 movie_id) {
    std::vector<SaveStateInfo> result;
    result.reserve(SaveStateSlotCount);
#endif
    for (u32 slot = 0; slot <= SaveStateSlotCount; ++slot) {
        const auto path = GetSaveStatePath(program_id, movie_id, slot);
        if (!FileUtil::Exists(path)) {
            continue;
        }

        SaveStateInfo info;
        info.slot = slot;

        FileUtil::IOFile file(path, "rb");
        if (!file) {
            LOG_ERROR(Core, "Could not open file {}", path);
            continue;
        }
        CSTHeader header;
        if (file.GetSize() < sizeof(header)) {
            LOG_ERROR(Core, "File too small {}", path);
#if TARGET_OS_IOS
            result.emplace_back(std::make_pair(
                std::move(info),
                fmt::format("{} is too small", Common::Log::TrimSourcePath(file.Filename()))));
#endif
            continue;
        }
        if (file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
            LOG_ERROR(Core, "Could not read from file {}", path);
#if TARGET_OS_IOS
            result.emplace_back(std::make_pair(
                std::move(info), fmt::format("{} does not have a valid header",
                                             Common::Log::TrimSourcePath(file.Filename()))));
#endif
            continue;
        }
        if (!ValidateSaveState(header, info, program_id, movie_id)) {
#if TARGET_OS_IOS
            result.emplace_back(std::make_pair(
                std::move(info), fmt::format("{} was created with a different build. Validation "
                                             "failed but the save was still made available",
                                             Common::Log::TrimSourcePath(file.Filename()))));
#endif
            continue;
        }

#if TARGET_OS_IOS
        result.emplace_back(std::make_pair(std::move(info), std::nullopt));
#else
        result.emplace_back(std::move(info));
#endif
    }
    return result;
}

void System::SaveState(u32 slot) const {
    if (app_loader) {
        if (!app_loader->SupportsSaveStates()) {
            throw std::runtime_error("The current app loader doesn't support save states");
        }
    }

    const u64 movie_id = movie.GetCurrentMovieID();
    const auto path = GetSaveStatePath(title_id, movie_id, slot);
    if (!FileUtil::CreateFullPath(path)) {
        throw std::runtime_error("Could not create path " + path);
    }

    // Stream the save instead of buffering the whole state in RAM. The previous path serialized the
    // entire system (FCRAM+VRAM+state) into a std::ostringstream, copied it again via .str(), then
    // allocated a zstd output buffer on top — a transient spike of ~2x uncompressed + compressed
    // (hundreds of MB) that jetsam-kills memory-tight devices on the quit auto-save. Instead:
    // serialize to a temp file (RAM stays flat as boost flushes to disk), then stream-compress it
    // into the .cst in fixed-size chunks. Peak extra RAM becomes a few small buffers, independent of
    // device or FCRAM size. The .cst format is unchanged (CSTHeader + a single zstd frame carrying
    // its content size), so LoadState is untouched.
    const auto tmp_path = path + ".uncompressed.tmp";
    struct TmpFileGuard {
        std::string path;
        ~TmpFileGuard() {
            if (FileUtil::Exists(path)) {
                FileUtil::Delete(path);
            }
        }
    } tmp_guard{tmp_path};

    // 1. Serialize the full system to the temp file.
    {
        std::ofstream ofs{tmp_path, std::ios_base::binary | std::ios_base::trunc};
        if (!ofs) {
            throw std::runtime_error("Could not open temp save file " + tmp_path);
        }
        oarchive oa{ofs};
        oa&* this;
    }
    const u64 uncompressed_size = FileUtil::GetSize(tmp_path);

    // 2. Build the header (unchanged format).
    CSTHeader header{};
    header.filetype = header_magic_bytes;
    header.program_id = title_id;
    std::string rev_bytes;
    CryptoPP::StringSource ss(Common::g_scm_rev, true,
                              new CryptoPP::HexDecoder(new CryptoPP::StringSink(rev_bytes)));
    std::memcpy(header.revision.data(), rev_bytes.data(), sizeof(header.revision));
    header.time = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    const std::string build_fullname = Common::g_build_fullname;
    std::memset(header.build_name.data(), 0, sizeof(header.build_name));
    std::memcpy(header.build_name.data(), build_fullname.c_str(),
                std::min(build_fullname.length(), sizeof(header.build_name) - 1));

    // 3. Write the header, then stream-compress the temp file into the .cst.
    FileUtil::IOFile file(path, "wb");
    if (!file) {
        throw std::runtime_error("Could not open file " + path);
    }
    if (file.WriteBytes(&header, sizeof(header)) != sizeof(header)) {
        throw std::runtime_error("Could not write to file " + path);
    }

    FileUtil::IOFile in_file(tmp_path, "rb");
    if (!in_file) {
        throw std::runtime_error("Could not reopen temp save file " + tmp_path);
    }

    ZSTD_CStream* const cstream = ZSTD_createCStream();
    if (cstream == nullptr) {
        throw std::runtime_error("Could not create ZSTD compression stream");
    }
    struct CStreamGuard {
        ZSTD_CStream* stream;
        ~CStreamGuard() {
            ZSTD_freeCStream(stream);
        }
    } cstream_guard{cstream};

    ZSTD_CCtx_setParameter(cstream, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
    // Record the content size in the frame header so DecompressDataZSTD (LoadState) can size its
    // output buffer, exactly as it does for CompressDataZSTDDefault today.
    ZSTD_CCtx_setPledgedSrcSize(cstream, uncompressed_size);

    const size_t in_capacity = ZSTD_CStreamInSize();
    const size_t out_capacity = ZSTD_CStreamOutSize();
    std::vector<u8> in_buffer(in_capacity);
    std::vector<u8> out_buffer(out_capacity);

    u64 remaining = uncompressed_size;
    bool finished = false;
    while (!finished) {
        const size_t to_read = static_cast<size_t>(std::min<u64>(in_capacity, remaining));
        if (to_read > 0 && in_file.ReadBytes(in_buffer.data(), to_read) != to_read) {
            throw std::runtime_error("Could not read temp save file " + tmp_path);
        }
        remaining -= to_read;
        const ZSTD_EndDirective mode = (remaining == 0) ? ZSTD_e_end : ZSTD_e_continue;

        ZSTD_inBuffer input = {in_buffer.data(), to_read, 0};
        bool chunk_done = false;
        while (!chunk_done) {
            ZSTD_outBuffer output = {out_buffer.data(), out_capacity, 0};
            const size_t ret = ZSTD_compressStream2(cstream, &output, &input, mode);
            if (ZSTD_isError(ret)) {
                throw std::runtime_error(std::string("ZSTD_compressStream2 error: ") +
                                         ZSTD_getErrorName(ret));
            }
            if (output.pos > 0 &&
                file.WriteBytes(out_buffer.data(), output.pos) != output.pos) {
                throw std::runtime_error("Could not write to file " + path);
            }
            if (mode == ZSTD_e_end) {
                finished = (ret == 0);
                chunk_done = finished;
            } else {
                chunk_done = (input.pos == input.size);
            }
        }
    }
}

void System::LoadState(u32 slot) {
    if (app_loader) {
        if (!app_loader->SupportsSaveStates()) {
            throw std::runtime_error("The current app loader doesn't support save states");
        }
    }
    if (Network::GetRoomMember().lock()->IsConnected()) {
        throw std::runtime_error("Unable to load while connected to multiplayer");
    }

    const u64 movie_id = movie.GetCurrentMovieID();
    const auto path = GetSaveStatePath(title_id, movie_id, slot);

    std::vector<u8> decompressed;
    {
        std::vector<u8> buffer(FileUtil::GetSize(path) - sizeof(CSTHeader));

        FileUtil::IOFile file(path, "rb");

        // load header
        CSTHeader header;
        if (file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
            throw std::runtime_error("Could not read from file at " + path);
        }

        // validate header
        SaveStateInfo info;
        info.slot = slot;
        if (!ValidateSaveState(header, info, title_id, movie_id)) {
            throw std::runtime_error("Invalid savestate");
        }

        if (file.ReadBytes(buffer.data(), buffer.size()) != buffer.size()) {
            throw std::runtime_error("Could not read from file at " + path);
        }
        decompressed = Common::Compression::DecompressDataZSTD(buffer);
    }
    std::istringstream sstream{
        std::string{reinterpret_cast<char*>(decompressed.data()), decompressed.size()},
        std::ios_base::binary};
    decompressed.clear();

    // Deserialize
    iarchive ia{sstream};
    ia&* this;
}

std::vector<u8> System::SaveStateBuffer() const {
    std::ostringstream sstream{std::ios_base::binary};
    // Serialize
    oarchive oa{sstream};
    oa&* this;

    const std::string& str{sstream.str()};
    const auto data = std::span<const u8>{reinterpret_cast<const u8*>(str.data()), str.size()};
    auto buffer = Common::Compression::CompressDataZSTDDefault(data);

    CSTHeader header{};
    header.filetype = header_magic_bytes;
    header.program_id = title_id;
    std::string rev_bytes;
    CryptoPP::StringSource ss(Common::g_scm_rev, true,
                              new CryptoPP::HexDecoder(new CryptoPP::StringSink(rev_bytes)));
    std::memcpy(header.revision.data(), rev_bytes.data(), sizeof(header.revision));
    header.time = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    const std::string build_fullname = Common::g_build_fullname;
    std::memset(header.build_name.data(), 0, sizeof(header.build_name));
    std::memcpy(header.build_name.data(), build_fullname.c_str(),
                std::min(build_fullname.length(), sizeof(header.build_name) - 1));

    std::vector<u8> result((u8*)&header, (u8*)&header + sizeof(header));
    std::copy(buffer.begin(), buffer.end(), std::back_inserter(result));

    return result;
}

bool System::LoadStateBuffer(std::vector<u8> buffer) {
    CSTHeader header;

    if (buffer.size() < sizeof(header)) {
        LOG_ERROR(Core, "Save state too small");
        return false;
    }

    header = *((CSTHeader*)buffer.data());

    if (header.filetype != header_magic_bytes) {
        LOG_ERROR(Core, "Invalid save state");
        return false;
    }

    if (header.program_id != title_id) {
        LOG_ERROR(Core, "Save state isn't for the current game");
        return false;
    }
    std::string revision = fmt::format("{:02x}", fmt::join(header.revision, ""));
    if (revision != Common::g_scm_rev) {
        LOG_ERROR(Core,
                  "Save state file created from a different revision (core: {}, savestate: {})",
                  Common::g_scm_rev, revision);
        return false;
    }

    std::vector<u8> state(buffer.begin() + sizeof(CSTHeader), buffer.end());
    auto decompressed = Common::Compression::DecompressDataZSTD(state);

    std::istringstream sstream{
        std::string{reinterpret_cast<char*>(decompressed.data()), decompressed.size()},
        std::ios_base::binary};
    decompressed.clear();

    // Deserialize
    iarchive ia{sstream};
    ia&* this;

    return true;
}

} // namespace Core
