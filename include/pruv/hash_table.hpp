/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <cstddef> // size_t

namespace pruv {

class hash_table {
public:
    class entry {
    public:
        size_t hash;

    private:
        entry *next;

        friend class hash_table;
    };

    class iterator {
    public:
        iterator(entry **prev = nullptr) : prev(prev) {}
        operator bool () const { return prev; }
        entry & operator * () const { return **prev; }
        entry * operator -> () const { return *prev; }
        template<typename T>
        T * get() const { return static_cast<T *>(*prev); }

    private:
        entry **prev;

        friend class hash_table;
    };

    ~hash_table();

    /// Place all entries into new table of size new_buckets_count.
    bool rehash(size_t new_buckets_count) noexcept;

    /// Enumerate all entries.
    void for_each(void *opaque, void (*visitor)(void *, entry *)) noexcept;

    template<typename T>
    void for_each(T &&f) noexcept
    {
        for_each(&f, [](void *f, entry *e) { (*reinterpret_cast<T *>(f))(e); });
    }

    /// Remove all entries from table and release internal memory.
    /// deleter called for each entry.
    void clear(void *opaque, void (*deleter)(void *, entry *)) noexcept;

    template<typename T>
    void clear(T &&f) noexcept
    {
        clear(&f, [](void *f, entry *e) { (*reinterpret_cast<T *>(f))(e); });
    }

    /// Insert hashed entry into table.
    iterator insert(entry *e) noexcept;

    /// Remove pointed entry from table.
    void remove(iterator it) noexcept;

    /// Find entry by hash and comparator.
    iterator find(size_t hash, void const *opaque,
            bool (*cmp)(void const *, entry const *)) noexcept;

    template<typename T>
    iterator find(size_t hash, T const &f) noexcept
    {
        return find(hash, &f, [](void const *f, entry const *e) -> bool {
                    return (*reinterpret_cast<T const *>(f))(e);
                });
    }

    bool empty() const noexcept { return _size == 0; }

    size_t size() const noexcept { return _size; }

    size_t buckets_count() const noexcept { return _buckets_count; }

private:
    entry **_t = nullptr;
    size_t _buckets_count = 0;
    size_t _size = 0;
};

} // namespace pruv
