#include "control/latch_store.hpp"

#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace anolis {
namespace control {

namespace {

constexpr int kSchemaVersion = 1;

// Flush a file's (or, on POSIX, a directory's) contents to storage.
bool sync_path(const std::filesystem::path &path, bool directory) {
#ifdef _WIN32
    // Windows cannot open a directory this way, and NTFS journals the rename.
    if (directory) {
        return true;
    }
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fd < 0) {
        return false;
    }
    const bool ok = _commit(fd) == 0;
    _close(fd);
    return ok;
#else
    const int fd = ::open(path.c_str(), (directory ? O_RDONLY | O_DIRECTORY : O_RDWR) | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

}  // namespace

LatchStore::LatchStore(std::filesystem::path file) : file_(std::move(file)) {}

std::filesystem::path LatchStore::path_for(const std::filesystem::path &data_dir, const std::string &runtime_name) {
    std::string name = runtime_name.empty() ? "default" : runtime_name;
    for (auto &c : name) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_' || c == '.';
        if (!safe) {
            c = '_';
        }
    }
    if (name == "." || name == "..") {
        name = "_";
    }
    return data_dir / "estop" / (name + ".json");
}

PersistedLatch LatchStore::load() const {
    PersistedLatch result;
    std::error_code ec;
    const bool exists = std::filesystem::exists(file_, ec);
    if (ec) {
        result.state = PersistedLatch::State::Unreadable;
        result.detail = std::format("the latch file {} could not be checked: {}", file_.string(), ec.message());
        return result;
    }
    if (!exists) {
        return result;
    }

    std::ifstream in(file_, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        result.state = PersistedLatch::State::Unreadable;
        result.detail = std::format("the latch file {} is present but could not be read", file_.string());
        return result;
    }

    const auto doc = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("latched") || !doc["latched"].is_boolean() ||
        !doc["latched"].get<bool>()) {
        result.state = PersistedLatch::State::Unreadable;
        result.detail = std::format("the latch file {} is present but not a valid latch record", file_.string());
        return result;
    }

    result.state = PersistedLatch::State::Latched;
    if (doc.contains("latched_at_epoch_ms") && doc["latched_at_epoch_ms"].is_number_integer()) {
        result.latched_at_epoch_ms = doc["latched_at_epoch_ms"].get<int64_t>();
    }
    result.detail = std::format("the latch file {} records a software stop", file_.string());
    return result;
}

bool LatchStore::save(int64_t latched_at_epoch_ms, const std::string &reason, std::string &error) const {
    const auto dir = file_.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = std::format("cannot create {}: {}", dir.string(), ec.message());
        return false;
    }

    const nlohmann::json doc = {{"schema_version", kSchemaVersion},
                                {"latched", true},
                                {"latched_at_epoch_ms", latched_at_epoch_ms},
                                {"reason", reason}};
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump() << '\n';
        out.flush();
        if (!out) {
            error = std::format("cannot write {}", tmp.string());
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!sync_path(tmp, false)) {
        error = std::format("cannot sync {}", tmp.string());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        error = std::format("cannot rename {} to {}: {}", tmp.string(), file_.string(), ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (!sync_path(dir, true)) {
        error = std::format("wrote {} but cannot sync {}", file_.string(), dir.string());
        return false;
    }
    return true;
}

bool LatchStore::remove(std::string &error) const {
    std::error_code ec;
    const bool removed = std::filesystem::remove(file_, ec);
    if (ec) {
        error = std::format("cannot remove {}: {}", file_.string(), ec.message());
        return false;
    }
    if (!removed) {
        return true;  // nothing was on disk, so nothing to make durable
    }
    if (!sync_path(file_.parent_path(), true)) {
        // The file is gone from the namespace; only its durability is in doubt.
        error = std::format("removed {} but cannot sync {}", file_.string(), file_.parent_path().string());
        return false;
    }
    return true;
}

}  // namespace control
}  // namespace anolis
