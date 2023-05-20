# Snapshots
An RAII interface for deferred reclamation, using a split reference counting scheme.

## Classes

the Snapshots header provides two classes:
1. `Snapshot::ReadPtr<T>`: While one of these is held, the data it points to will not be deleted.
2. `Snapshot::Source<T>`: a provider of ReadPtrs

`Snapshot::Source` provides methods to get a current value, set a new value, and clean up stale values.

## Motivation

This interface is thematically similar to the proposed `std::snapshot_ptr`/`std::snapshot_source`, but the fact that `Snapshot::Source` provides a dedicated `cleanse` method means programmers can decide which thread(s) are supposed to delete unused values.

Setting a new value and cleaning up stale values are mutually exclusive, and use a mutex to enforce this.
However, getting a snapshot of the current data performs a single `fetch_add` to read an index and increment a reference count.

This interface may be useful if reading a snapshot needs to be lockfree, for example in realtime/deadline-oriented code.
A similar interface can be implemented using epoch-based approaches, but that generally requires registering readers in-advancce, whereas reference counting generalizes well to runaway or recursive reader counts.

## Example Usage

Setting a new value:
```
using Strings = std::vector<std::string>;
Snapshot::Source<Strings> currentStrings;

Strings s{"Hello", "World"};
currentStrings.set(std::make_unique<Strings>(s));
```

Reading the current value:
```
Snapshot::ReadPtr<Strings> now = currentStrings.get();
for (const auto& str : *now)
{
    std::cout << str << std::endl;
}
```

Cleaning up stale/outdated values that are unused:
```
currentStrings.cleanse();
```

## Modifications

Internally, this implementation stores data in a linked list, which gets linearly scanned.

This provides reference stability, but can be very inefficient if plenty of stale data is allowed to build up.

A different container can be used, with better performance, and retain any deadline safety properties, so long as lookup-by-index is not blocked by adding elements.