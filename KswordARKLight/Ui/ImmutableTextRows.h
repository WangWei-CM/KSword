#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Ksword::Ui {

// Native read-only table projection: snapshots and permutations share complete
// immutable row values. A caller that appends first detaches only row refs.
class ImmutableTextRows final {
public:
    using Row = std::vector<std::wstring>;
    using Values = std::vector<Row>;
    using RowRef = std::shared_ptr<const Row>;
    using Refs = std::vector<RowRef>;
    using Snapshot = std::shared_ptr<const Refs>;
    class const_iterator final {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;
        const_iterator() = default;
        explicit const_iterator(Refs::const_iterator value) : value_(value) {}
        reference operator*() const { return **value_; }
        pointer operator->() const { return value_->get(); }
        const_iterator& operator++() { ++value_; return *this; }
        const_iterator operator++(int) { auto old=*this; ++*this; return old; }
        friend bool operator==(const const_iterator&, const const_iterator&) = default;
    private:
        Refs::const_iterator value_;
    };

    ImmutableTextRows() : rows_(emptyRows()) {}
    ImmutableTextRows(const ImmutableTextRows&) = default;
    ImmutableTextRows& operator=(const ImmutableTextRows&) = default;
    ImmutableTextRows(ImmutableTextRows&& other) noexcept : rows_(std::move(other.rows_)), flat_(std::move(other.flat_)) {
        other.rows_=emptyRows();
    }
    ImmutableTextRows& operator=(ImmutableTextRows&& other) noexcept {
        if(this!=&other){rows_=std::move(other.rows_);flat_=std::move(other.flat_);other.rows_=emptyRows();}
        return *this;
    }
    explicit ImmutableTextRows(const Values& values) : ImmutableTextRows() { assign(values); }
    explicit ImmutableTextRows(Values&& values) : ImmutableTextRows() { assign(std::move(values)); }
    ImmutableTextRows& operator=(const Values& values) { assign(values); return *this; }
    ImmutableTextRows& operator=(Values&& values) { assign(std::move(values)); return *this; }
    bool empty() const noexcept { return rows_->empty(); }
    std::size_t size() const noexcept { return rows_->size(); }
    const Row& operator[](std::size_t index) const { return *(*rows_)[index]; }
    const_iterator begin() const { return const_iterator(rows_->begin()); }
    const_iterator end() const { return const_iterator(rows_->end()); }
    void clear() { rows_=emptyRows(); flat_.reset(); }
    void reserve(std::size_t count) { detach(); rows_->reserve(count); }
    void push_back(const Row& row) { detach(); rows_->push_back(std::make_shared<const Row>(row)); }
    void push_back(Row&& row) { detach(); rows_->push_back(std::make_shared<const Row>(std::move(row))); }
    Snapshot snapshot() const noexcept { return rows_; }

    static ImmutableTextRows reorder(const Snapshot& source, const std::vector<std::size_t>& order) {
        ImmutableTextRows value;
        value.rows_=std::make_shared<Refs>();
        value.rows_->reserve(order.size());
        for(const auto index:order) value.rows_->push_back((*source)[index]);
        return value;
    }

    // Compatibility for explicit legacy report/serializer consumers. Hot native
    // capture/sort uses snapshot() and never asks to materialize a text graph.
    operator const Values&() const {
        return *contiguousSnapshot();
    }
    std::shared_ptr<const Values> contiguousSnapshot() const {
        if(!flat_){
            auto flat=std::make_shared<Values>();flat->reserve(rows_->size());
            for(const auto& row:*rows_) flat->push_back(*row);
            flat_=std::move(flat);
        }
        return flat_;
    }
private:
    static std::shared_ptr<Refs> emptyRows(){static const auto empty=std::make_shared<Refs>();return empty;}
    void detach(){flat_.reset();if(rows_.use_count()!=1)rows_=std::make_shared<Refs>(*rows_);}
    void assign(const Values& values){auto refs=std::make_shared<Refs>();refs->reserve(values.size());for(const auto& row:values)refs->push_back(std::make_shared<const Row>(row));rows_=std::move(refs);flat_.reset();}
    void assign(Values&& values){auto refs=std::make_shared<Refs>();refs->reserve(values.size());for(auto& row:values)refs->push_back(std::make_shared<const Row>(std::move(row)));rows_=std::move(refs);flat_.reset();}
    std::shared_ptr<Refs> rows_;
    mutable std::shared_ptr<const Values> flat_;
};

} // namespace Ksword::Ui
