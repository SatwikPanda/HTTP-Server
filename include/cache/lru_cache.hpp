#pragma once

#include "cache/cache.hpp"

#include <list>
#include <mutex>
#include <unordered_map>

namespace cache {

class LruCache final : public Cache {

public:

    explicit LruCache(
        std::size_t capacity
    );

    std::optional<http::Response>
    get(const std::string& key) override;

    void put(
        const std::string& key,
        const http::Response& response
    ) override;

    void remove(
        const std::string& key
    ) override;

    void clear() override;

private:

    struct Entry {

        std::string key;

        http::Response response;
    };

    using List =
        std::list<Entry>;

    using Iterator =
        List::iterator;

    std::size_t capacity_;

    List entries_;

    std::unordered_map<
        std::string,
        Iterator
    > lookup_;

    mutable std::mutex mutex_;
};

}