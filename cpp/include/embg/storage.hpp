#pragma once

// Fixed-capacity storage primitives for static-allocation mode.
//
// These provide std-like APIs but use no heap — storage is embedded in the
// object. They are only used when StaticConfig is active. In default mode,
// the core headers alias directly to std::* equivalents.
//
// Design notes:
//   - StaticString: null-terminated fixed buffer, implicit const char* ctor
//     so add_node("preprocess", ...) compiles unchanged.
//   - StaticMap: flat linear array, O(N) lookup (N is tiny, ≤16).
//   - StaticVector: std::array + size, no push_back beyond capacity.
//   - Function: SBO callable — stores lambda inline up to InlineBytes.
//     static_assert gives a clear error if a capture is too big.

#include "config.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace embg {

// ─── StaticString ─────────────────────────────────────────────────────────────

template<std::size_t N>
class StaticString {
public:
    StaticString() noexcept { buf_[0] = '\0'; }

    StaticString(const char* s) noexcept {
        if (!s) { buf_[0] = '\0'; return; }
        std::size_t i = 0;
        for (; i < N && s[i] != '\0'; ++i) buf_[i] = s[i];
        buf_[i] = '\0';
    }

    StaticString(const std::string& s) noexcept {
        std::size_t i = 0;
        for (; i < N && i < s.size(); ++i) buf_[i] = s[i];
        buf_[i] = '\0';
    }

    StaticString(std::string_view sv) noexcept {
        std::size_t i = 0;
        for (; i < N && i < sv.size(); ++i) buf_[i] = sv[i];
        buf_[i] = '\0';
    }

    operator std::string_view() const noexcept { return {buf_, size()}; }
    operator std::string() const { return {buf_, size()}; }

    const char* c_str() const noexcept { return buf_; }
    std::size_t size() const noexcept { return std::strlen(buf_); }
    bool empty() const noexcept { return buf_[0] == '\0'; }

    bool operator==(const char* other) const noexcept {
        return std::strncmp(buf_, other, N) == 0;
    }
    bool operator==(std::string_view other) const noexcept {
        return std::string_view(buf_, size()) == other;
    }
    bool operator==(const StaticString& other) const noexcept {
        return std::strncmp(buf_, other.buf_, N) == 0;
    }
    bool operator==(const std::string& other) const noexcept {
        return other == buf_;
    }

    std::size_t find(const char* sub, std::size_t pos = 0) const noexcept {
        const char* p = std::strstr(buf_ + pos, sub);
        return p ? static_cast<std::size_t>(p - buf_) : npos;
    }
    std::size_t find(char c, std::size_t pos = 0) const noexcept {
        for (std::size_t i = pos; i < size(); ++i)
            if (buf_[i] == c) return i;
        return npos;
    }

    StaticString substr(std::size_t pos, std::size_t len = npos) const noexcept {
        StaticString out;
        std::size_t sz = size();
        if (pos >= sz) return out;
        std::size_t copy = (len == npos || pos + len > sz) ? sz - pos : len;
        std::size_t i = 0;
        for (; i < N && i < copy; ++i) out.buf_[i] = buf_[pos + i];
        out.buf_[i] = '\0';
        return out;
    }

    StaticString& operator+=(const char* s) noexcept {
        std::size_t cur = size();
        std::size_t i = 0;
        for (; cur + i < N && s[i] != '\0'; ++i) buf_[cur + i] = s[i];
        buf_[cur + i] = '\0';
        return *this;
    }
    StaticString& operator+=(char c) noexcept {
        std::size_t cur = size();
        if (cur < N) { buf_[cur] = c; buf_[cur + 1] = '\0'; }
        return *this;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    char buf_[N + 1];  // +1 for null terminator
};

// Free-operator overloads for symmetry
template<std::size_t N>
bool operator==(const char* a, const StaticString<N>& b) noexcept { return b == a; }
template<std::size_t N>
bool operator==(const std::string& a, const StaticString<N>& b) noexcept { return b == a; }

template<std::size_t N>
std::ostream& operator<<(std::ostream& os, const StaticString<N>& s) {
    return os << s.c_str();
}

// ─── StaticVector ─────────────────────────────────────────────────────────────

template<typename T, std::size_t Cap>
class StaticVector {
public:
    StaticVector() noexcept : size_(0) {}

    void push_back(T v) {
        if (size_ < Cap)
            data_[size_++] = std::move(v);
        else
            throw std::runtime_error("StaticVector: capacity exceeded");
    }

    void pop_back() noexcept { if (size_ > 0) --size_; }

    void clear() noexcept { size_ = 0; }
    void resize(std::size_t n) noexcept { size_ = (n <= Cap) ? n : Cap; }

    T&       operator[](std::size_t i)       { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }

    T&       back()       { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }

    std::size_t size()     const noexcept { return size_; }
    bool        empty()    const noexcept { return size_ == 0; }
    static constexpr std::size_t capacity() noexcept { return Cap; }

    T*       begin()       { return data_.data(); }
    T*       end()         { return data_.data() + size_; }
    const T* begin() const { return data_.data(); }
    const T* end()   const { return data_.data() + size_; }

    void reverse() noexcept {
        for (std::size_t i = 0, j = size_; i + 1 < j; ++i, --j)
            std::swap(data_[i], data_[j - 1]);
    }

private:
    std::array<T, Cap> data_;
    std::size_t        size_;
};

// ─── StaticMap ────────────────────────────────────────────────────────────────
// Flat array of key-value pairs. Linear scan — fine for small N (≤16).

template<typename K, typename V, std::size_t Cap>
class StaticMap {
public:
    StaticMap() noexcept : size_(0) {}

    StaticMap(std::initializer_list<std::pair<K, V>> init) noexcept : size_(0) {
        for (const auto& entry : init) {
            if (size_ < Cap) {
                data_[size_].first  = entry.first;
                data_[size_].second = entry.second;
                ++size_;
            }
        }
    }

    void insert_or_assign(K key, V val) {
        for (std::size_t i = 0; i < size_; ++i) {
            if (data_[i].first == key) {
                data_[i].second = std::move(val);
                return;
            }
        }
        if (size_ < Cap) {
            data_[size_].first  = std::move(key);
            data_[size_].second = std::move(val);
            ++size_;
        } else {
            throw std::runtime_error("StaticMap: capacity exceeded");
        }
    }

    // find returns pointer to pair — compatible with it->second pattern.
    // end() returns nullptr — so `it == end()` works as `ptr == nullptr`.
    std::pair<K, V>* find(const K& key) noexcept {
        for (std::size_t i = 0; i < size_; ++i)
            if (data_[i].first == key) return &data_[i];
        return nullptr;
    }
    const std::pair<K, V>* find(const K& key) const noexcept {
        for (std::size_t i = 0; i < size_; ++i)
            if (data_[i].first == key) return &data_[i];
        return nullptr;
    }

    static constexpr nullptr_t end() noexcept { return nullptr; }

    std::size_t size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }

    auto begin()       { return data_.data(); }
    auto end_ptr()     { return data_.data() + size_; }
    auto begin() const { return data_.data(); }
    auto end_ptr() const { return data_.data() + size_; }

private:
    std::array<std::pair<K, V>, Cap> data_;
    std::size_t                      size_;
};

// ─── Function<Sig, InlineBytes> — SBO callable ────────────────────────────────
//
// Stores a callable inline (small-buffer optimisation). No heap allocation.
// If a lambda's capture exceeds InlineBytes, a static_assert fires with a
// message telling the user to bump FnInlineBytes in their config.

template<typename Sig, std::size_t InlineBytes>
class Function;

template<typename Ret, typename... Args, std::size_t InlineBytes>
class Function<Ret(Args...), InlineBytes> {
public:
    Function() noexcept : invoke_(nullptr) {}

    Function(std::nullptr_t) noexcept : invoke_(nullptr) {}

    template<typename F>
        requires (!std::is_same_v<std::decay_t<F>, Function>)
    Function(F&& f) {
        using Decayed = std::decay_t<F>;
        static_assert(sizeof(Decayed) <= InlineBytes,
            "embg::Function: callable capture too large — "
            "increase FnInlineBytes in your config");
        static_assert(alignof(Decayed) <= alignof(std::max_align_t),
            "embg::Function: callable alignment too high");
        new (&storage_) Decayed(std::forward<F>(f));
        invoke_ = &invoke_stub<Decayed>;
        destroy_ = &destroy_stub<Decayed>;
        copy_ = &copy_stub<Decayed>;
    }

    Function(const Function& other) : invoke_(other.invoke_) {
        if (other.invoke_ && other.copy_) {
            other.copy_(&other.storage_, &storage_);
            destroy_ = other.destroy_;
            copy_ = other.copy_;
        }
    }

    Function(Function&& other) noexcept : invoke_(other.invoke_) {
        if (other.invoke_) {
            std::memcpy(&storage_, &other.storage_, InlineBytes);
            destroy_ = other.destroy_;
            copy_ = other.copy_;
            other.invoke_ = nullptr;
        }
    }

    Function& operator=(const Function& other) {
        if (this != &other) {
            reset();
            if (other.invoke_ && other.copy_) {
                other.copy_(&other.storage_, &storage_);
                invoke_ = other.invoke_;
                destroy_ = other.destroy_;
                copy_ = other.copy_;
            }
        }
        return *this;
    }

    Function& operator=(Function&& other) noexcept {
        if (this != &other) {
            reset();
            if (other.invoke_) {
                std::memcpy(&storage_, &other.storage_, InlineBytes);
                invoke_ = other.invoke_;
                destroy_ = other.destroy_;
                copy_ = other.copy_;
                other.invoke_ = nullptr;
            }
        }
        return *this;
    }

    ~Function() { reset(); }

    explicit operator bool() const noexcept { return invoke_ != nullptr; }

    Ret operator()(Args... args) const {
        if (!invoke_)
            throw std::runtime_error("embg::Function: call on empty function");
        return invoke_(&storage_, std::forward<Args>(args)...);
    }

private:
    alignas(std::max_align_t) std::byte storage_[InlineBytes] = {};
    Ret   (*invoke_)(const void*, Args&&...) = nullptr;
    void  (*destroy_)(void*)                 = nullptr;
    void  (*copy_)(const void*, void*)       = nullptr;

    void reset() noexcept {
        if (invoke_ && destroy_) destroy_(&storage_);
        invoke_ = nullptr;
    }

    template<typename F>
    static Ret invoke_stub(const void* obj, Args&&... args) {
        // const_cast: operator() is const to allow calling from std::visit,
        // but the stored callable may be mutable (e.g. make_node's lambda).
        // This matches std::function's internal behavior.
        return (*static_cast<F*>(const_cast<void*>(obj)))(std::forward<Args>(args)...);
    }

    template<typename F>
    static void destroy_stub(void* obj) {
        static_cast<F*>(obj)->~F();
    }

    template<typename F>
    static void copy_stub(const void* src, void* dst) {
        new (dst) F(*static_cast<const F*>(src));
    }
};

} // namespace embg
