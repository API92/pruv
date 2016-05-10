/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <cinttypes>
#include <cstddef>
#include <iterator>

namespace pruv {

template<typename T>
class list_node {
public:
    template<typename U>
    struct iterator_base : public std::iterator<
                               std::bidirectional_iterator_tag, U> {
        typedef typename std::conditional<
            std::is_const<U>::value,
            const list_node, list_node>::type base_type;
        iterator_base(base_type *p = nullptr) : p(p) {}
        U & operator * ();
        U * operator -> ();
        iterator_base & operator++();
        iterator_base operator++(int);
        iterator_base & operator--();
        iterator_base operator--(int);
        bool operator == (const iterator_base &rhs) const;
        bool operator != (const iterator_base &rhs) const;

        base_type *p;
    };

    typedef iterator_base<T> iterator;
    typedef iterator_base<const T> const_iterator;

    list_node() :
        next(this),
        prev(this)
    {
    }

    bool empty() const
    {
        return next == this;
    }

    iterator begin()
    {
        return iterator(next);
    }

    const_iterator begin() const
    {
        return const_iterator(next);
    }

    iterator end()
    {
        return iterator(this);
    }

    const_iterator end() const
    {
        return const_iterator(this);
    }

    std::reverse_iterator<iterator> rbegin()
    {
        return std::reverse_iterator<iterator>(end());
    }

    std::reverse_iterator<iterator> rend()
    {
        return std::reverse_iterator<iterator>(begin());
    }

    T & front()
    {
        return *next->get();
    }

    const T & front() const
    {
        return *next->get();
    }

    T & back()
    {
        return *prev->get();
    }

    const T & back() const
    {
        return *prev->get();
    }

    void push_front(list_node *other)
    {
        other->next = next;
        next->prev = other;
        next = other;
        other->prev = this;
    }

    void push_back(list_node *other)
    {
        other->next = this;
        other->prev = prev;
        prev->next = other;
        prev = other;
    }

    void remove_from_list()
    {
        next->prev = prev;
        prev->next = next;
        next = prev = this;
    }

private:
    T * get()
    {
        // automatically subtract offset, if list_node is base of T.
        return static_cast<T *>(this);
    }

    const T * get() const
    {
        // automatically subtract offset, if list_node is base of T.
        return static_cast<const T *>(this);
    }

    list_node *next;
    list_node *prev;
};

template<typename T>
template<typename U>
U & list_node<T>::iterator_base<U>::operator * ()
{
    return *p->get();
}

template<typename T>
template<typename U>
U * list_node<T>::iterator_base<U>::operator -> ()
{
    return p->get();
}

template<typename T>
template<typename U>
typename list_node<T>::template iterator_base<U> &
list_node<T>::iterator_base<U>::operator++()
{
    p = p->next;
    return *this;
}

template<typename T>
template<typename U>
typename list_node<T>::template iterator_base<U>
list_node<T>::iterator_base<U>::operator++(int)
{
    iterator_base res = *this;
    p = p->next;
    return res;
}

template<typename T>
template<typename U>
typename list_node<T>::template iterator_base<U> &
list_node<T>::iterator_base<U>::operator--()
{
    p = p->prev;
    return *this;
}

template<typename T>
template<typename U>
typename list_node<T>::template iterator_base<U>
list_node<T>::iterator_base<U>::operator--(int)
{
    iterator_base res = *this;
    p = p->prev;
    return res;
}

template<typename T>
template<typename U>
bool list_node<T>::iterator_base<U>::operator == (const iterator_base &rhs) const
{
    return p == rhs.p;
}

template<typename T>
template<typename U>
bool list_node<T>::iterator_base<U>::operator != (const iterator_base &rhs) const
{
    return p != rhs.p;
}

} // namespace pruv
