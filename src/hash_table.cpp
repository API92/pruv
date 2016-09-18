/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/hash_table.hpp>

#include <algorithm>
#include <new>

#include <pruv/log.hpp>

namespace pruv {

hash_table::~hash_table()
{
    if (_size)
        pruv_log(LOG_ERR, "Destructing non-empty hash_table. Memory leaks.");
    delete _t;
}

bool hash_table::rehash(size_t new_buckets_count) noexcept
{
    entry **new_t = nullptr;
    if (new_buckets_count) {
        new_t = new (std::nothrow) entry * [new_buckets_count]();
        if (!new_t) {
            pruv_log(LOG_EMERG, "Can't allocate memory for table.");
            return false;
        }
    }
    else if (_size) {
        pruv_log(LOG_ERR, "Can't place some entries into empty table.");
        return false;
    }

    if (new_t && _t)
        for (size_t i = 0; i < _buckets_count; ++i)
            for (entry *p = _t[i]; p;) {
                entry *&to = new_t[p->hash % new_buckets_count];
                entry *next = p->next;
                p->next = to;
                to = p;
                p = next;
            }

    delete _t;
    _t = new_t;
    _buckets_count = new_buckets_count;
    return true;
}

void hash_table::for_each(void *opaque, void (*visitor)(void *, entry *))
    noexcept
{
    if (_t && _size)
        for (size_t i = 0; i < _buckets_count; ++i)
            for (entry *p = _t[i]; p;) {
                entry *next = p->next;
                visitor(opaque, p);
                p = next;
            }
}

void hash_table::clear(void *opaque, void (*deleter)(void *, entry *)) noexcept
{
    for_each(opaque, deleter);
    delete _t;
    _t = nullptr;
    _buckets_count = 0;
    _size = 0;
}

hash_table::iterator hash_table::insert(entry *e) noexcept
{
    if (_size >= _buckets_count && !rehash(std::max<size_t>(1, 2 * _size)))
        return iterator();

    size_t bucket = e->hash % _buckets_count;
    e->next = _t[bucket];
    _t[bucket] = e;
    ++_size;
    return iterator(&_t[bucket]);
}

void hash_table::remove(iterator it) noexcept
{
    *it.prev = (*it.prev)->next;
    --_size;
}

hash_table::iterator hash_table::find(size_t hash, void const *opaque,
        bool (*cmp)(void const *, entry const *)) noexcept
{
    if (!_t || !_size)
        return iterator();

    size_t bucket = hash % _buckets_count;
    for (entry **e = &_t[bucket]; *e; e = &(*e)->next)
        if (cmp(opaque, *e))
            return iterator(e);

    return iterator();
}

} // namespace pruv
