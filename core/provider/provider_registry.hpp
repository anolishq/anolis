#pragma once

/**
 * @file provider_registry.hpp
 * @brief Thread-safe registry of live provider handles.
 */

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "i_provider_handle.hpp"

namespace anolis {
namespace provider {

/**
 * @brief Thread-safe registry of provider instances known to the runtime.
 *
 * ProviderRegistry is the shared lookup point for long-lived provider handles
 * used by polling, HTTP handlers, automation, and supervision code.
 *
 * Threading:
 * Concurrent reads are allowed via shared locking. Writes serialize provider
 * add, remove, and clear operations.
 *
 * Ownership:
 * The registry stores shared ownership of each provider handle. Readers receive
 * copied `std::shared_ptr` instances, so an in-flight user can finish safely
 * even if the registry entry is replaced or removed later.
 */
class ProviderRegistry {
public:
    ProviderRegistry() = default;
    ~ProviderRegistry() = default;

    // Non-copyable, non-movable (manages mutex)
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;
    ProviderRegistry(ProviderRegistry&&) = delete;
    ProviderRegistry& operator=(ProviderRegistry&&) = delete;

    /**
     * @brief Add or replace a provider handle by ID.
     *
     * Replacing an existing entry does not invalidate `std::shared_ptr`
     * instances that were already handed out to readers.
     *
     * @param provider_id Unique provider identifier
     * @param provider Shared pointer to provider handle (must not be null)
     */
    void add_provider(const std::string& provider_id, std::shared_ptr<IProviderHandle> provider);

    /**
     * @brief Remove a provider entry from the registry.
     *
     * Ownership:
     * Removing the entry only drops the registry's ownership. Existing shared
     * pointers returned earlier remain valid until their last holder releases
     * them.
     *
     * @param provider_id Unique provider identifier
     * @return true if provider was removed, false if not found
     */
    bool remove_provider(const std::string& provider_id);

    /**
     * @brief Get a provider handle by ID.
     *
     * Thread Safety:
     * Multiple threads may call this concurrently.
     *
     * @param provider_id Unique provider identifier
     * @return Shared pointer to provider (nullptr if not found)
     */
    std::shared_ptr<IProviderHandle> get_provider(const std::string& provider_id) const;

    /**
     * @brief Get all providers as a point-in-time snapshot.
     *
     * The returned map can be iterated safely after later registry mutations
     * because both the container and the provider handles are copied by value.
     *
     * Performance:
     * O(n) copy. Prefer caching the result when a caller needs repeated
     * iteration over a stable provider set.
     *
     * @return Copy of provider map (provider_id -> IProviderHandle)
     */
    std::unordered_map<std::string, std::shared_ptr<IProviderHandle>> get_all_providers() const;

    /**
     * @brief Get provider IDs as a snapshot.
     *
     * @return Vector of provider IDs
     */
    std::vector<std::string> get_provider_ids() const;

    /**
     * @brief Check if a provider exists
     *
     * Thread-safe existence check without retrieving the provider.
     *
     * @param provider_id Unique provider identifier
     * @return true if provider exists
     */
    bool has_provider(const std::string& provider_id) const;

    /** @brief Get the current provider count. */
    size_t provider_count() const;

    /**
     * @brief Remove all provider entries.
     *
     * This is primarily used during shutdown or full runtime teardown.
     */
    void clear();

private:
    // Provider storage with thread-safe access
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<IProviderHandle>> providers_;
};

}  // namespace provider
}  // namespace anolis
