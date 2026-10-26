#include "cache/storage/lru_storage.hpp"

#include "http/date.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <list>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace cache::storage {
namespace {
constexpr std::size_t body_overhead = sizeof(Body) + 256;
constexpr std::size_t entry_overhead = sizeof(Entry) + 512;
constexpr std::size_t reader_cost = 128;
core::Error invalid() { return core::make_error(core::ErrorCategory::invalid_argument, "cache storage"); }
core::Error allocation_error() { return core::make_error(core::ErrorCategory::system_error, "cache allocation"); }
core::Error capacity_error() { return core::make_error(core::ErrorCategory::would_block, "cache capacity"); }
core::Error oversized_error() { return core::make_error(core::ErrorCategory::invalid_argument, "cache object limit"); }

// A process-wide sequence also separates observations from different stores.
EntryVersion next_version() {
    static std::atomic<std::uint64_t> sequence{1};
    auto value = sequence.load();
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (sequence.compare_exchange_weak(value, value + 1)) return {value};
    }
    return {}; // Never wrap, even across clear or remove/reinsert.
}

struct Domain {
    explicit Domain(Limits value) : limits(value), budget(value.total_bytes) {}
    Limits limits;
    core::MemoryBudget budget;
    std::atomic<std::size_t> readers{};
};

struct BodyAllocation {
    std::shared_ptr<const Body> body;
    std::mutex mutex;
    std::size_t candidates{}, residents{};
    void change(core::AllocationKind from, core::AllocationKind to) {
        std::lock_guard lock(mutex);
        if (from == core::AllocationKind::candidate) --candidates;
        if (from == core::AllocationKind::resident) --residents;
        if (to == core::AllocationKind::candidate) ++candidates;
        if (to == core::AllocationKind::resident) ++residents;
        body->reclassify(residents ? core::AllocationKind::resident :
            candidates ? core::AllocationKind::candidate : core::AllocationKind::pinned);
    }
};

struct EntryOwner {
    std::shared_ptr<Domain> domain;
    std::shared_ptr<BodyAllocation> allocation;
    core::AllocationKind kind{core::AllocationKind::candidate};
    Entry entry;
    ~EntryOwner() {
        if (allocation) allocation->change(kind, core::AllocationKind::pinned);
        // Release owned dynamic metadata before returning its reservation.
        entry.headers = http::HeaderMap{};
        std::string().swap(entry.key.scheme);
        std::string().swap(entry.key.authority.host);
        std::string().swap(entry.key.path_query);
        std::string().swap(entry.content_encoding);
        entry.etag.reset();
    }
    void classify(core::AllocationKind value) {
        if (allocation) allocation->change(kind, value);
        kind = value;
        entry.metadata.reclassify(value);
    }
};
struct EntryDelete {
    std::shared_ptr<EntryOwner> owner;
    void operator()(const Entry*) noexcept {} // owner owns the aliased object
};
struct PinDelete {
    Snapshot entry;
    std::shared_ptr<Domain> domain;
    core::Reservation reservation;
    void operator()(const Entry*) noexcept { --domain->readers; }
};
std::shared_ptr<EntryOwner> owner_of(const Snapshot& entry) {
    if (const auto* pin = std::get_deleter<PinDelete>(entry)) return owner_of(pin->entry);
    if (const auto* value = std::get_deleter<EntryDelete>(entry)) return value->owner;
    return {};
}

bool forbidden(std::string_view name) {
    for (const auto field : {"Connection", "Keep-Alive", "Proxy-Authenticate", "Proxy-Authorization",
            "TE", "Trailer", "Transfer-Encoding", "Upgrade", "Content-Length", "Proxy-Connection"}) {
        if (http::header_name_equals(name, field)) return true;
    }
    return false;
}
core::Result<std::size_t> metadata_cost(const Key& key, const http::HeaderMap& headers, std::size_t limit) {
    std::size_t cost = entry_overhead;
    auto add = [&](std::size_t size) {
        if (cost > limit || size > (limit - cost) / 4) return false;
        // Logical allowance includes copied string capacity, map key and validators.
        cost += size * 4;
        return true;
    };
    if (key.scheme != "http" || key.authority.host.empty() || key.path_query.empty() ||
        key.authority.host.size() > 8192 || key.path_query.size() > 8192 ||
        !add(key.scheme.size() + 32) || !add(key.authority.host.size() + 32) ||
        !add(key.path_query.size() + 32) || headers.size() > 100) return oversized_error();
    for (const auto& field : headers) {
        if (forbidden(field.name) || !http::is_valid_header_name(field.name) ||
            !http::is_valid_header_value(field.value)) return invalid();
        if (!add(sizeof(http::HeaderField)) || !add(field.name.size()) || !add(32) ||
            !add(field.value.size()) || !add(32)) return oversized_error();
    }
    if (cost > limit) return oversized_error();
    return cost;
}

core::Result<std::shared_ptr<EntryOwner>> make_entry(const std::shared_ptr<Domain>& domain,
    const Key& key, const http::HeaderMap& headers, Timing timing, int status,
    std::shared_ptr<BodyAllocation> body = {}) {
    auto cost = metadata_cost(key, headers, domain->limits.metadata_bytes);
    if (!cost) return cost.error();
    auto charge = domain->budget.reserve(cost.value());
    if (!charge) return charge.error();
    try {
        auto owner = std::make_shared<EntryOwner>();
        owner->domain = domain;
        owner->entry.metadata = std::move(charge.value());
        owner->entry.key = key;
        owner->entry.headers = headers;
        owner->entry.timing = timing;
        owner->entry.status = status;
        if (auto encoding = headers.get("Content-Encoding")) owner->entry.content_encoding = *encoding;
        if (auto tag = headers.get("ETag")) {
            auto parsed = http::parse_validator_list(*tag);
            if (parsed && !parsed->wildcard && parsed->tags.size() == 1 && headers.get_all("ETag").size() == 1)
                owner->entry.etag = parsed->tags.front();
        }
        if (auto modified = headers.get("Last-Modified")) {
            auto parsed = http::parse_http_date(*modified, timing.final_head_wall);
            if (parsed && headers.get_all("Last-Modified").size() == 1) owner->entry.last_modified = parsed.value();
        }
        if (body) {
            body->change(core::AllocationKind::pinned, core::AllocationKind::candidate);
            owner->allocation = std::move(body);
            owner->entry.body = std::shared_ptr<const Body>(owner->allocation, owner->allocation->body.get());
        }
        return owner;
    } catch (const std::bad_alloc&) { return allocation_error(); }
}
Snapshot publish(std::shared_ptr<EntryOwner> owner) {
    const auto* entry = &owner->entry;
    return Snapshot(entry, EntryDelete{std::move(owner)});
}
std::size_t cost_of(const Entry& entry) { return entry.metadata.size() + entry.body->accounted_bytes(); }

// KeyHash's public compatibility implementation builds a string. Hash fields
// directly here so lookup/erase never allocate while holding the storage lock.
struct Hash {
    std::size_t operator()(const Key& key) const noexcept {
        auto hash = std::hash<std::string>{}(key.scheme);
        const auto mix = [&](std::size_t value) { hash ^= value + 0x9e3779b9U + (hash << 6) + (hash >> 2); };
        mix(std::hash<std::string>{}(key.authority.host));
        mix(key.authority.port); mix(key.authority.ipv6);
        mix(std::hash<std::string>{}(key.path_query));
        return hash;
    }
};
} // namespace

struct Candidate::Impl {
    std::shared_ptr<EntryOwner> owner;
    core::Reservation charge;
    std::vector<std::byte> bytes;
};
Candidate::Candidate(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Candidate::~Candidate() = default;
Candidate::Candidate(Candidate&&) noexcept = default;
Candidate& Candidate::operator=(Candidate&&) noexcept = default;
core::Result<void> Candidate::append(std::span<const std::byte> bytes) {
    if (!impl_) return invalid();
    if (bytes.empty()) return {};
    const auto& domain = impl_->owner->domain;
    if (bytes.size() > domain->limits.object_bytes - impl_->bytes.size()) return oversized_error();
    const auto size = impl_->bytes.size() + bytes.size();
    // Reserve the complete new allocation while the old buffer is still charged.
    auto charge = domain->budget.reserve(size + body_overhead);
    if (!charge) return charge.error();
    try {
        std::vector<std::byte> next(size);
        if (next.capacity() > size) return capacity_error();
        std::copy(impl_->bytes.begin(), impl_->bytes.end(), next.begin());
        std::copy(bytes.begin(), bytes.end(), next.begin() + static_cast<std::ptrdiff_t>(impl_->bytes.size()));
        impl_->bytes.swap(next);
        // Free the old allocation before its reservation is released.
        std::vector<std::byte>().swap(next);
        impl_->charge = std::move(charge.value());
        return {};
    } catch (const std::bad_alloc&) { return allocation_error(); }
}
core::Result<Snapshot> Candidate::finish() {
    if (!impl_) return invalid();
    try {
        auto allocation = std::make_shared<BodyAllocation>();
        auto body = Body::create(std::move(impl_->bytes), std::move(impl_->charge));
        if (!body) return body.error();
        allocation->body = std::move(body.value());
        allocation->candidates = 1;
        impl_->owner->allocation = std::move(allocation);
        impl_->owner->entry.body = std::shared_ptr<const Body>(impl_->owner->allocation, impl_->owner->allocation->body.get());
        auto owner = std::move(impl_->owner);
        impl_.reset(); // A failed publication consumes the candidate and releases it.
        return publish(std::move(owner));
    } catch (const std::bad_alloc&) { impl_.reset(); return allocation_error(); }
}

struct LruStorage::Impl {
    explicit Impl(std::shared_ptr<Domain> value) : domain(std::move(value)), epoch(next_version()) {}
    std::shared_ptr<Domain> domain;
    core::Reservation buckets;
    mutable std::mutex mutex;
    std::list<Snapshot> recency; // front is most recently used
    std::unordered_map<Key, std::list<Snapshot>::iterator, Hash> index;
    EntryVersion epoch;
    std::size_t resident_bytes{};

    EntryVersion observed(const Key& key) const {
        const auto found = index.find(key);
        return found == index.end() ? epoch : (*found->second)->version;
    }
    void retire(std::list<Snapshot>::iterator position, std::list<Snapshot>& retired) {
        resident_bytes -= cost_of(**position);
        owner_of(*position)->classify(core::AllocationKind::pinned);
        index.erase((*position)->key);
        retired.splice(retired.end(), recency, position);
    }
};
LruStorage::LruStorage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
core::Result<std::unique_ptr<LruStorage>> LruStorage::create(Limits limits) {
    if (!limits.entries || limits.entries > 1000000 || !limits.readers || !limits.object_bytes ||
        limits.object_bytes > limits.resident_bytes || limits.resident_bytes > limits.total_bytes ||
        limits.total_bytes > std::numeric_limits<std::size_t>::max() / 8 || limits.metadata_bytes < entry_overhead)
        return invalid();
    try {
        auto impl = std::make_unique<Impl>(std::make_shared<Domain>(limits));
        if (!impl->epoch.value) return capacity_error();
        auto buckets = impl->domain->budget.reserve((limits.entries + 1) * 4 * sizeof(void*) + 512, core::AllocationKind::metadata);
        if (!buckets) return buckets.error();
        impl->buckets = std::move(buckets.value());
        impl->index.reserve(limits.entries + 1);
        return std::unique_ptr<LruStorage>(new LruStorage(std::move(impl)));
    } catch (const std::bad_alloc&) { return allocation_error(); }
}
LruStorage::~LruStorage() {
    // Destruction requires callers to have joined; readers/candidates may outlive us.
    for (const auto& entry : impl_->recency) owner_of(entry)->classify(core::AllocationKind::pinned);
}
core::Result<Candidate> LruStorage::begin(const Key& key, const http::HeaderMap& headers, Timing timing, int status) {
    auto owner = make_entry(impl_->domain, key, headers, timing, status);
    if (!owner) return owner.error();
    auto charge = impl_->domain->budget.reserve(body_overhead);
    if (!charge) return charge.error();
    try {
        auto builder = std::make_unique<Candidate::Impl>();
        builder->owner = std::move(owner.value());
        builder->charge = std::move(charge.value());
        return Candidate(std::move(builder));
    } catch (const std::bad_alloc&) { return allocation_error(); }
}
core::Result<Snapshot> LruStorage::refresh(const Snapshot& selected, const http::HeaderMap& headers, Timing timing) {
    auto old = owner_of(selected);
    if (!old || old->domain != impl_->domain || !selected->body) return invalid();
    auto owner = make_entry(impl_->domain, selected->key, headers, timing, selected->status, old->allocation);
    if (!owner) return owner.error();
    try { return publish(std::move(owner.value())); }
    catch (const std::bad_alloc&) { return allocation_error(); }
}
LookupResult LruStorage::lookup(const Key& key) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->index.find(key);
    if (found == impl_->index.end()) return {StorageOutcome::miss, {}, impl_->epoch, {}};
    const auto& entry = *found->second;
    auto count = impl_->domain->readers.load();
    do {
        if (count >= impl_->domain->limits.readers) return {StorageOutcome::exhausted, {}, entry->version, {}};
    } while (!impl_->domain->readers.compare_exchange_weak(count, count + 1));
    auto charge = impl_->domain->budget.reserve(reader_cost, core::AllocationKind::temporary);
    if (!charge) { --impl_->domain->readers; return {StorageOutcome::exhausted, {}, entry->version, {}}; }
    try {
        // shared_ptr invokes the deleter on control-block allocation failure too.
        Snapshot pin(entry.get(), PinDelete{entry, impl_->domain, std::move(charge.value())});
        impl_->recency.splice(impl_->recency.begin(), impl_->recency, found->second);
        return {StorageOutcome::hit, std::move(pin), entry->version, {}};
    } catch (const std::bad_alloc&) { return {StorageOutcome::failed, {}, entry->version, allocation_error()}; }
}
StoreResult LruStorage::replace(const Key& key, EntryVersion expected, Snapshot candidate) {
    auto source = owner_of(candidate);
    if (!source || source->domain != impl_->domain || !candidate->body || candidate->key != key)
        return {StorageOutcome::failed, {}, invalid()};
    const auto cost = cost_of(*candidate);
    if (candidate->body->bytes().size() > impl_->domain->limits.object_bytes || cost > impl_->domain->limits.resident_bytes)
        return {StorageOutcome::oversized, {}, {}};
    auto prepared = make_entry(impl_->domain, key, candidate->headers, candidate->timing, candidate->status, source->allocation);
    if (!prepared) return {prepared.error().category == core::ErrorCategory::would_block ? StorageOutcome::exhausted : StorageOutcome::failed, {}, prepared.error()};
    std::list<Snapshot> pending, retired;
    try { pending.push_back(publish(prepared.value())); }
    catch (const std::bad_alloc&) { return {StorageOutcome::failed, {}, allocation_error()}; }
    std::unique_lock lock(impl_->mutex);
    if (impl_->observed(key) != expected) return {StorageOutcome::version_conflict, impl_->observed(key), {}};
    const auto version = next_version();
    if (!version.value) return {StorageOutcome::exhausted, {}, {}};
    auto found = impl_->index.find(key);
    if (found == impl_->index.end()) {
        try { found = impl_->index.emplace(key, pending.begin()).first; }
        catch (const std::bad_alloc&) { return {StorageOutcome::failed, {}, allocation_error()}; }
    } else {
        // Reuse the map node; no throwing operation follows this point.
        const auto old = found->second;
        impl_->resident_bytes -= cost_of(**old);
        owner_of(*old)->classify(core::AllocationKind::pinned);
        retired.splice(retired.end(), impl_->recency, old);
        found->second = pending.begin();
    }
    prepared.value()->entry.version = version;
    prepared.value()->classify(core::AllocationKind::resident);
    impl_->recency.splice(impl_->recency.begin(), pending);
    impl_->resident_bytes += cost;
    impl_->epoch = version;
    while (impl_->index.size() > impl_->domain->limits.entries || impl_->resident_bytes > impl_->domain->limits.resident_bytes)
        impl_->retire(std::prev(impl_->recency.end()), retired);
    lock.unlock(); // Retired bodies and candidate temporaries are destroyed outside the lock.
    return {StorageOutcome::stored, version, {}};
}
StoreResult LruStorage::erase(const Key& key, EntryVersion expected) {
    std::list<Snapshot> retired;
    std::unique_lock lock(impl_->mutex);
    if (impl_->observed(key) != expected) return {StorageOutcome::version_conflict, impl_->observed(key), {}};
    const auto version = next_version();
    if (!version.value) return {StorageOutcome::exhausted, {}, {}};
    const auto found = impl_->index.find(key);
    const bool present = found != impl_->index.end();
    if (present) impl_->retire(found->second, retired);
    impl_->epoch = version;
    lock.unlock();
    return {present ? StorageOutcome::stored : StorageOutcome::miss, version, {}};
}
core::Result<void> LruStorage::clear() {
    std::list<Snapshot> retired;
    std::unique_lock lock(impl_->mutex);
    const auto version = next_version();
    if (!version.value) return capacity_error();
    while (!impl_->recency.empty()) impl_->retire(impl_->recency.begin(), retired);
    impl_->epoch = version;
    lock.unlock();
    return {};
}
Statistics LruStorage::statistics() const {
    std::lock_guard lock(impl_->mutex);
    const auto& budget = impl_->domain->budget;
    return {impl_->index.size(), impl_->resident_bytes, budget.used(),
        budget.used(core::AllocationKind::candidate), budget.used(core::AllocationKind::pinned),
        budget.used(core::AllocationKind::temporary), impl_->domain->readers.load()};
}
} // namespace cache::storage
