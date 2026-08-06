// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <streambuf>
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

/// Deletes the scratch `.part` the save writes next to the `.cst`, on every exit path (including
/// the throws below).
struct TmpFileGuard {
    std::string path;
    ~TmpFileGuard() {
        if (FileUtil::Exists(path)) {
            FileUtil::Delete(path);
        }
    }
};

/// Growable `std::vector<u8>` sink for the boost archive. One buffer, no copies: the archive writes
/// straight into the vector we later compress from.
///
/// The pre-existing code serialized into an `ostringstream` and then copied the whole thing out with
/// `.str()` — that copy is what made the save's peak ~2x the uncompressed state. A brief detour
/// routed the archive to a temp FILE instead, which fixed the memory but made the save several times
/// slower (two full disk passes over hundreds of MB) and it silently blew past the frontend's
/// completion timeout. This is the settlement: 1x uncompressed, at memory speed.
class VectorOutputBuf : public std::streambuf {
public:
    explicit VectorOutputBuf(std::vector<u8>& target) : buffer{target} {}

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        buffer.insert(buffer.end(), reinterpret_cast<const u8*>(data),
                      reinterpret_cast<const u8*>(data) + count);
        return count;
    }

    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            buffer.push_back(static_cast<u8>(ch));
        }
        return ch;
    }

private:
    std::vector<u8>& buffer;
};

/// Read-only view over an existing buffer for the boost archive — `sgetn` becomes a memcpy from
/// memory already resident, which is what an `istringstream` gave us before and a disk-backed
/// `ifstream` (4-8 KB buffer, one syscall storm for the many small reads a binary archive issues)
/// very much did not.
class SpanInputBuf : public std::streambuf {
public:
    SpanInputBuf(u8* base, std::size_t size) {
        char* const begin = reinterpret_cast<char*>(base);
        setg(begin, begin, begin + size);
    }
};

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

/// `reason`, when non-null, receives a frontend-presentable description of WHY the state was
/// rejected. Without it every rejection collapses into a single opaque "Invalid savestate", which
/// is useless for diagnosing a resume that silently didn't apply.
static bool ValidateSaveState(const CSTHeader& header, SaveStateInfo& info, u64 program_id,
                              u64 movie_id, std::string* reason = nullptr) {
    const auto path = GetSaveStatePath(program_id, movie_id, info.slot);
    const auto reject = [reason](std::string message) {
        if (reason) {
            *reason = std::move(message);
        }
        return false;
    };

    if (header.filetype != header_magic_bytes) {
        LOG_WARNING(Core, "Invalid save state file {}", path);
        return reject("Not a save state file (bad header magic)");
    }
    info.time = header.time;

    if (header.program_id != program_id) {
        LOG_WARNING(Core, "Save state file isn't for the current game {}", path);
        return reject(fmt::format("Save state is for title {:016X}, but {:016X} is running",
                                  static_cast<u64>(header.program_id), program_id));
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
    return reject(fmt::format("Save state was made by a different build (state: {}, this build: {})",
                              revision, Common::g_scm_rev));
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

    // ONE in-memory buffer, then stream-compress out of it. Peak is 1x the uncompressed state plus a
    // couple of fixed zstd buffers:
    //   * The original code serialized into an ostringstream and copied it out with `.str()` before
    //     compressing — ~2x uncompressed + compressed, which jetsam-killed memory-tight devices on
    //     the quit auto-save.
    //   * The first fix routed the archive to a temp FILE. RAM went flat, but the save then made two
    //     full disk passes over hundreds of MB and took long enough to blow the frontend's
    //     completion timeout outright.
    // This keeps half the original's memory saving at memory speed. The .cst format is unchanged
    // (CSTHeader + a single zstd frame carrying its content size).
    std::vector<u8> uncompressed;

    // Compress into a `.part` and rename it into place at the very end, so the `.cst` NEVER exists
    // on disk in a partial state. Streaming the compressed side made this mandatory: the output is
    // written incrementally, and every consumer of these files decides "this state is ready" by
    // looking at the filesystem — `stateExists:`/`saveStates:` check existence, and the iOS
    // frontend's save shim used to poll for a non-empty file and copy the first bytes it saw. That
    // produced valid-header/truncated-payload `.cst`s that saved "successfully" and then failed to
    // load. `rename(2)` over an existing destination is atomic, so presence now genuinely implies
    // completeness.
    const auto part_path = path + ".part";
    TmpFileGuard part_guard{part_path};

    // 1. Serialize the full system into the buffer.
    const auto serialize_start = std::chrono::steady_clock::now();
    {
        VectorOutputBuf sink{uncompressed};
        std::ostream os{&sink};
        oarchive oa{os};
        oa&* this;
    }
    const u64 uncompressed_size = uncompressed.size();
    const auto serialize_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - serialize_start)
                                  .count();

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

    // 3. Write the header, then stream-compress the buffer into the .part. Scoped so the file is
    //    closed (and flushed) before the rename below.
    const auto compress_start = std::chrono::steady_clock::now();
    {
        FileUtil::IOFile file(part_path, "wb");
        if (!file) {
            throw std::runtime_error("Could not open file " + part_path);
        }
        if (file.WriteBytes(&header, sizeof(header)) != sizeof(header)) {
            throw std::runtime_error("Could not write to file " + part_path);
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
        std::vector<u8> out_buffer(out_capacity);

        u64 offset = 0;
        u64 remaining = uncompressed_size;
        bool finished = false;
        while (!finished) {
            const size_t to_read = static_cast<size_t>(std::min<u64>(in_capacity, remaining));
            u8* const chunk = uncompressed.data() + offset;
            offset += to_read;
            remaining -= to_read;
            const ZSTD_EndDirective mode = (remaining == 0) ? ZSTD_e_end : ZSTD_e_continue;

            ZSTD_inBuffer input = {chunk, to_read, 0};
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
                    throw std::runtime_error("Could not write to file " + part_path);
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

    const auto compress_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - compress_start)
                                 .count();

    // 4. Atomically publish. Only now does the .cst exist (or change), and only ever complete.
    if (!FileUtil::Rename(part_path, path)) {
        throw std::runtime_error("Could not move " + part_path + " into place at " + path);
    }

    LOG_INFO(Core, "Saved state slot {}: serialize {}ms, compress {}ms, {} MB uncompressed", slot,
             serialize_ms, compress_ms, uncompressed_size / (1024 * 1024));
    save_state_timings = fmt::format("{} MB, serialize {}ms, compress {}ms",
                                     uncompressed_size / (1024 * 1024), serialize_ms, compress_ms);
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

    // Mirror of the save: decompress into ONE buffer, then deserialize over it in place.
    //   * The original held THREE copies at once — the compressed buffer, DecompressDataZSTD's full
    //     output, and a second full copy, because the istringstream was constructed from a
    //     std::string built off that output BEFORE it was cleared.
    //   * The first fix decompressed to a temp file and deserialized from an ifstream. RAM went
    //     flat, but a binary archive issues an enormous number of small reads, and through a 4-8 KB
    //     file buffer that is orders of magnitude slower than reading resident memory — the load
    //     stopped finishing inside any reasonable timeout.
    // Now: 1x uncompressed plus a fixed chunk buffer, read at memcpy speed. Format unchanged.
    const u64 file_size = FileUtil::GetSize(path);
    // Also guards the old `GetSize(path) - sizeof(CSTHeader)` underflow: a missing file gave 0, and
    // 0u64 - 256 sized a vector at ~1.8e19 bytes.
    if (file_size <= sizeof(CSTHeader)) {
        throw std::runtime_error("Save state file is missing or truncated: " + path);
    }

    std::vector<u8> decompressed;
    const auto decompress_start = std::chrono::steady_clock::now();
    {
        FileUtil::IOFile file(path, "rb");
        if (!file) {
            throw std::runtime_error("Could not open file at " + path);
        }

        // load header
        CSTHeader header;
        if (file.ReadBytes(&header, sizeof(header)) != sizeof(header)) {
            throw std::runtime_error("Could not read from file at " + path);
        }

        // validate header — the reason is propagated so the frontend can tell a stale-build state
        // (permanent: don't retry, pick another) from a transient failure.
        SaveStateInfo info;
        info.slot = slot;
        std::string reason;
        if (!ValidateSaveState(header, info, title_id, movie_id, &reason)) {
            throw std::runtime_error(reason.empty() ? "Invalid savestate" : reason);
        }

        ZSTD_DStream* const dstream = ZSTD_createDStream();
        if (dstream == nullptr) {
            throw std::runtime_error("Could not create ZSTD decompression stream");
        }
        struct DStreamGuard {
            ZSTD_DStream* stream;
            ~DStreamGuard() {
                ZSTD_freeDStream(stream);
            }
        } dstream_guard{dstream};

        const size_t in_capacity = ZSTD_DStreamInSize();
        const size_t out_capacity = ZSTD_DStreamOutSize();
        std::vector<u8> in_buffer(in_capacity);
        std::vector<u8> out_buffer(out_capacity);

        u64 remaining = file_size - sizeof(CSTHeader);
        bool frame_complete = false;
        bool reserved = false;
        while (remaining > 0 && !frame_complete) {
            const size_t to_read = static_cast<size_t>(std::min<u64>(in_capacity, remaining));
            if (file.ReadBytes(in_buffer.data(), to_read) != to_read) {
                throw std::runtime_error("Could not read from file at " + path);
            }
            // The save pledges the content size, so the very first chunk lets us size the output
            // exactly — one allocation instead of a series of doubling reallocs, each of which would
            // briefly hold both the old and new buffer.
            if (!reserved) {
                reserved = true;
                const unsigned long long content_size =
                    ZSTD_getFrameContentSize(in_buffer.data(), to_read);
                if (content_size != ZSTD_CONTENTSIZE_UNKNOWN &&
                    content_size != ZSTD_CONTENTSIZE_ERROR) {
                    decompressed.reserve(static_cast<size_t>(content_size));
                }
            }
            remaining -= to_read;

            ZSTD_inBuffer input = {in_buffer.data(), to_read, 0};
            while (input.pos < input.size) {
                ZSTD_outBuffer output = {out_buffer.data(), out_capacity, 0};
                const size_t ret = ZSTD_decompressStream(dstream, &output, &input);
                if (ZSTD_isError(ret)) {
                    throw std::runtime_error(std::string("ZSTD_decompressStream error: ") +
                                             ZSTD_getErrorName(ret));
                }
                if (output.pos > 0) {
                    decompressed.insert(decompressed.end(), out_buffer.data(),
                                        out_buffer.data() + output.pos);
                }
                // ret == 0 means the frame ended; anything after it is trailing padding we ignore.
                if (ret == 0) {
                    frame_complete = true;
                    break;
                }
            }
        }
        if (!frame_complete) {
            throw std::runtime_error("Save state payload is truncated: " + path);
        }
    }
    const auto decompress_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - decompress_start)
                                   .count();

    // Deserialize over the buffer in place — no second copy, no file round-trip.
    //
    // NOTE: this is the expensive half, and not because of the archive. `System::serialize`'s load
    // branch calls Shutdown(true) + Init(), which tears down and rebuilds the entire Vulkan stack
    // (instance, device, swapchain, shader compiles) and then reloads the per-title pipeline cache.
    // The emulation thread is blocked for all of it. See the timings below before optimising here.
    const auto deserialize_start = std::chrono::steady_clock::now();
    {
        SpanInputBuf source{decompressed.data(), decompressed.size()};
        std::istream is{&source};
        iarchive ia{is};
        ia&* this;
    }
    const auto deserialize_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - deserialize_start)
                                    .count();

    LOG_INFO(Core, "Loaded state slot {}: decompress {}ms, deserialize {}ms, {} MB uncompressed",
             slot, decompress_ms, deserialize_ms, decompressed.size() / (1024 * 1024));
    // `serialize` filled in the shutdown/init/disk-resources breakdown during the deserialize above.
    save_state_timings =
        fmt::format("{} MB, decompress {}ms, deserialize {}ms [{}]",
                    decompressed.size() / (1024 * 1024), decompress_ms, deserialize_ms,
                    save_state_timings);
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
