#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace Snapshot
{

namespace Detail
{

template<typename T>
static constexpr bool IsLockFree = std::atomic<T>::is_always_lock_free;

using UWide = std::conditional_t<IsLockFree<uint64_t>, uint64_t, uint32_t>;
using UThin = std::conditional_t<IsLockFree<uint64_t>, uint32_t, uint16_t>;
static constexpr UWide ThinBits = IsLockFree<uint64_t> ? 32 : 16;
static_assert(IsLockFree<UWide>);

template<typename T>
struct Inner
{
    std::unique_ptr<T> data;
    mutable std::atomic<UThin> refCount{0};
};

static constexpr UWide IndexBits = std::numeric_limits<UThin>::max();
static constexpr UWide RefCountBits = ~IndexBits;

class Outer
{
    UWide value = 0;
public:
    Outer() = default;
    Outer(UWide v) : value(v) {}
    Outer(const Outer&) = default;
    Outer& operator=(const Outer&) = default;
    ~Outer() = default;

    operator UWide() const noexcept { return value; }
    UThin index() const noexcept { return value & IndexBits; }
    UThin refCount() const noexcept { return (value & RefCountBits) >> ThinBits; }
};

template<typename T>
class List
{
    std::unique_ptr<T> data;
    std::atomic<List*> next{nullptr};

    explicit List(std::unique_ptr<T> d) : data(std::move(d)) {}
public:
    List() = default;
    List(const List&) = delete;
    List(List&&) = delete;
    List& operator=(const List&) = delete;
    List& operator=(List&&) = delete;
    ~List() { std::unique_ptr<List> toDelete{next.load()}; }

    List* advance() noexcept { return next.load(); }
    const List* advance() const noexcept { return next.load(); }

    std::unique_ptr<T>& get() noexcept { return data; }

    const T* at(UThin index) const noexcept
    {
        if (index == 0)
            return data.get();

        const List* nextPtr = advance();
        if (!nextPtr)
            return nullptr;

        return nextPtr->at(index - 1);
    }

    UThin push(std::unique_ptr<T> element, UThin index = 0)
    {
        if (index >= IndexBits)
            std::terminate();

        if (!data)
        {
            data = std::move(element);
            return index;
        }

        List* nextPtr = advance();
        if (nextPtr)
            return nextPtr->push(std::move(element), index + 1);
        
        nextPtr = new List(std::move(element));
        next.store(nextPtr);
        return index + 1;
    }
};

} // namespace Detail

template<typename T>
class ReadPtr
{
    const Detail::Inner<T>* data = nullptr;
public:
    ReadPtr() = default;

    ReadPtr(const Detail::Inner<T>* ptr)
        : data(ptr)
    {
    }

    ReadPtr(const ReadPtr&) = delete;
    ReadPtr& operator=(const ReadPtr&) = delete;

    void swap(ReadPtr& other) noexcept { std::swap(data, other.data); }

    ReadPtr(ReadPtr&& other) noexcept
        : data(std::exchange(other.data, nullptr))
    {
    }

    ReadPtr& operator=(ReadPtr&& other) noexcept
    {
        ReadPtr temp(std::move(other));
        swap(temp);
        return *this;
    }

    void reset() noexcept
    {
        if (data)
            data->refCount--;

        data = nullptr;
    }

    ~ReadPtr() noexcept { reset(); }

    T* get() const noexcept { return data ? data->data.get() : nullptr; }
    operator bool() const noexcept { return get() != nullptr; }
    T* operator->() const noexcept { return get(); }
    T& operator*() const noexcept { return *get(); }
};

template<typename T>
class Source
{
    using UWide = Detail::UWide;
    using UThin = Detail::UThin;
    using Inner = Detail::Inner<T>;
    using Outer = Detail::Outer;
    using List = Detail::List<Inner>;

    static constexpr UWide NilIndex = Detail::IndexBits;
    static constexpr UWide AddRef = NilIndex + 1; 

    std::mutex guard;
    mutable std::atomic<UWide> current{NilIndex};
    List objects;
public:
    Source() = default;
    Source(const Source&) = delete;
    Source(Source&&) = delete;
    Source& operator=(const Source&) = delete;
    Source& operator=(Source&&) = delete;
    ~Source() = default;

    explicit Source(std::unique_ptr<T> initialValue) { set(std::move(initialValue)); }

    ReadPtr<T> get() const
    {
        Outer now = current.fetch_add(AddRef);
        return ReadPtr<T>(objects.at(now.index()));
    }

    void set(std::unique_ptr<T> newValue)
    {
        auto wrapper = std::make_unique<Inner>();
        wrapper->data = std::move(newValue);

        std::scoped_lock lock(guard);

        UThin index = objects.push(std::move(wrapper));
        Outer old = current.exchange(index);
        const Inner* stale = objects.at(old.index());
        if (stale)
            stale->refCount += old.refCount();
    }

    void cleanse()
    {
        std::scoped_lock lock(guard);
        Outer live = current.load();
        UThin index = 0;
        for (List* node = &objects; node != nullptr; node = node->advance(), ++index)
        {
            if (index == live.index())
                continue;

            std::unique_ptr<Inner>& stale = node->get();
            if (!stale)
                continue;
            
            if (stale->refCount.load() == 0)
                stale.reset();
        }
    }
};

} // namespace Snapshot