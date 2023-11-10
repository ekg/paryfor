#pragma once

// atomic_queue
// Copyright (c) 2019 Maxim Egorushkin. MIT License
// see LICENSE.atomic_queue for full license

#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <thread>
#include <functional>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || \
    defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
namespace paryfor {
namespace atomic_queue {
constexpr int CACHE_LINE_SIZE = 64;
static inline void spin_loop_pause() noexcept {
    _mm_pause();
}
} // namespace atomic_queue
} // namespace paryfor
#elif defined(__arm__) || defined(__aarch64__)
// TODO: These need to be verified as I do not have access to ARM platform.
namespace paryfor {
namespace atomic_queue {
constexpr int CACHE_LINE_SIZE = 64;
static inline void spin_loop_pause() noexcept {
#if (defined(__ARM_ARCH_6K__) || \
     defined(__ARM_ARCH_6Z__) || \
     defined(__ARM_ARCH_6ZK__) || \
     defined(__ARM_ARCH_6T2__) || \
     defined(__ARM_ARCH_7__) || \
     defined(__ARM_ARCH_7A__) || \
     defined(__ARM_ARCH_7R__) || \
     defined(__ARM_ARCH_7M__) || \
     defined(__ARM_ARCH_7S__) || \
     defined(__ARM_ARCH_8A__) || \
     defined(__aarch64__))
    asm volatile ("yield" ::: "memory");
#else
    asm volatile ("nop" ::: "memory");
#endif
}
} // namespace atomic_queue
} // namespace paryfor
#elif defined(__riscv) && (__riscv_xlen == 64)
namespace paryfor {
namespace atomic_queue {
constexpr int CACHE_LINE_SIZE = 64;
static inline void spin_loop_pause() noexcept {
    asm volatile ("nop" ::: "memory");
}
} // namespace atomic_queue
} // namespace paryfor
#else
#error "Unknown CPU architecture."
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace paryfor {
namespace atomic_queue {

#if defined(__GNUC__) || defined(__clang__)
#define ATOMIC_QUEUE_LIKELY(expr) __builtin_expect(static_cast<bool>(expr), 1)
#define ATOMIC_QUEUE_UNLIKELY(expr) __builtin_expect(static_cast<bool>(expr), 0)
#else
#define ATOMIC_QUEUE_LIKELY(expr) expr
#define ATOMIC_QUEUE_UNLIKELY(expr) expr
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

auto constexpr A = std::memory_order_acquire;
auto constexpr R = std::memory_order_release;
auto constexpr X = std::memory_order_relaxed;
auto constexpr C = std::memory_order_seq_cst;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace atomic_queue
} // namespace paryfor

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace paryfor {
namespace atomic_queue {

using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace details {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<size_t elements_per_cache_line> struct GetCacheLineIndexBits { static int constexpr value = 0; };
template<> struct GetCacheLineIndexBits<64> { static int constexpr value = 6; };
template<> struct GetCacheLineIndexBits<32> { static int constexpr value = 5; };
template<> struct GetCacheLineIndexBits<16> { static int constexpr value = 4; };
template<> struct GetCacheLineIndexBits< 8> { static int constexpr value = 3; };
template<> struct GetCacheLineIndexBits< 4> { static int constexpr value = 2; };
template<> struct GetCacheLineIndexBits< 2> { static int constexpr value = 1; };

template<bool minimize_contention, unsigned array_size, size_t elements_per_cache_line>
struct GetIndexShuffleBits {
    static int constexpr bits = GetCacheLineIndexBits<elements_per_cache_line>::value;
    static unsigned constexpr min_size = 1u << (bits * 2);
    static int constexpr value = array_size < min_size ? 0 : bits;
};

template<unsigned array_size, size_t elements_per_cache_line>
struct GetIndexShuffleBits<false, array_size, elements_per_cache_line> {
    static int constexpr value = 0;
};

// Multiple writers/readers contend on the same cache line when storing/loading elements at
// subsequent indexes, aka false sharing. For power of 2 ring buffer size it is possible to re-map
// the index in such a way that each subsequent element resides on another cache line, which
// minimizes contention. This is done by swapping the lowest order N bits (which are the index of
// the element within the cache line) with the next N bits (which are the index of the cache line)
// of the element index.
template<int BITS>
constexpr unsigned remap_index(unsigned index) noexcept {
    constexpr unsigned MASK = (1u << BITS) - 1;
    unsigned mix = (index ^ (index >> BITS)) & MASK;
    return index ^ mix ^ (mix << BITS);
}

template<>
constexpr unsigned remap_index<0>(unsigned index) noexcept {
    return index;
}

template<int BITS, class T>
constexpr T& map(T* elements, unsigned index) noexcept {
    index = remap_index<BITS>(index);
    return elements[index];
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Implement a "bit-twiddling hack" for finding the next power of 2
// in either 32 bits or 64 bits in C++11 compatible constexpr functions

// "Runtime" version for 32 bits
// --a;
// a |= a >> 1;
// a |= a >> 2;
// a |= a >> 4;
// a |= a >> 8;
// a |= a >> 16;
// ++a;

template<class T>
constexpr T decrement(T x) {
    return x - 1;
}

template<class T>
constexpr T increment(T x) {
    return x + 1;
}

template<class T>
constexpr T or_equal(T x, unsigned u) {
    return (x | x >> u);
}

template<class T, class... Args>
constexpr T or_equal(T x, unsigned u, Args... rest) {
    return or_equal(or_equal(x, u), rest...);
}

constexpr uint32_t round_up_to_power_of_2(uint32_t a) noexcept {
    return increment(or_equal(decrement(a), 1, 2, 4, 8, 16));
}

constexpr uint64_t round_up_to_power_of_2(uint64_t a) noexcept {
    return increment(or_equal(decrement(a), 1, 2, 4, 8, 16, 32));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace details

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Derived>
class AtomicQueueCommon {
protected:
    // Put these on different cache lines to avoid false sharing between readers and writers.
    alignas(CACHE_LINE_SIZE) std::atomic<unsigned> head_ = {};
    alignas(CACHE_LINE_SIZE) std::atomic<unsigned> tail_ = {};

    // The special member functions are not thread-safe.

    AtomicQueueCommon() noexcept = default;

    AtomicQueueCommon(AtomicQueueCommon const& b) noexcept
        : head_(b.head_.load(X))
        , tail_(b.tail_.load(X)) {}

    AtomicQueueCommon& operator=(AtomicQueueCommon const& b) noexcept {
        head_.store(b.head_.load(X), X);
        tail_.store(b.tail_.load(X), X);
        return *this;
    }

    void swap(AtomicQueueCommon& b) noexcept {
        unsigned h = head_.load(X);
        unsigned t = tail_.load(X);
        head_.store(b.head_.load(X), X);
        tail_.store(b.tail_.load(X), X);
        b.head_.store(h, X);
        b.tail_.store(t, X);
    }

    template<class T, T NIL>
    static T do_pop_atomic(std::atomic<T>& q_element) noexcept {
        if(Derived::spsc_) {
            for(;;) {
                T element = q_element.load(X);
                if(ATOMIC_QUEUE_LIKELY(element != NIL)) {
                    q_element.store(NIL, R);
                    return element;
                }
                if(Derived::maximize_throughput_)
                    spin_loop_pause();
            }
        }
        else {
            for(;;) {
                T element = q_element.exchange(NIL, R); // (2) The store to wait for.
                if(ATOMIC_QUEUE_LIKELY(element != NIL))
                    return element;
                // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
                do
                    spin_loop_pause();
                while(Derived::maximize_throughput_ && q_element.load(X) == NIL);
            }
        }
    }

    template<class T, T NIL>
    static void do_push_atomic(T element, std::atomic<T>& q_element) noexcept {
        assert(element != NIL);
        if(Derived::spsc_) {
            while(ATOMIC_QUEUE_UNLIKELY(q_element.load(X) != NIL))
                if(Derived::maximize_throughput_)
                    spin_loop_pause();
            q_element.store(element, R);
        }
        else {
            for(T expected = NIL; ATOMIC_QUEUE_UNLIKELY(!q_element.compare_exchange_strong(expected, element, R, X)); expected = NIL) {
                do
                    spin_loop_pause(); // (1) Wait for store (2) to complete.
                while(Derived::maximize_throughput_ && q_element.load(X) != NIL);
            }
        }
    }

    enum State : unsigned char { EMPTY, STORING, STORED, LOADING };

    template<class T>
    static T do_pop_any(std::atomic<unsigned char>& state, T& q_element) noexcept {
        if(Derived::spsc_) {
            while(ATOMIC_QUEUE_UNLIKELY(state.load(A) != STORED))
                if(Derived::maximize_throughput_)
                    spin_loop_pause();
            T element{std::move(q_element)};
            state.store(EMPTY, R);
            return element;
        }
        else {
            for(;;) {
                unsigned char expected = STORED;
                if(ATOMIC_QUEUE_LIKELY(state.compare_exchange_strong(expected, LOADING, X, X))) {
                    T element{std::move(q_element)};
                    state.store(EMPTY, R);
                    return element;
                }
                // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
                do
                    spin_loop_pause();
                while(Derived::maximize_throughput_ && state.load(X) != STORED);
            }
        }
    }

    template<class U, class T>
    static void do_push_any(U&& element, std::atomic<unsigned char>& state, T& q_element) noexcept {
        if(Derived::spsc_) {
            while(ATOMIC_QUEUE_UNLIKELY(state.load(A) != EMPTY))
                if(Derived::maximize_throughput_)
                    spin_loop_pause();
            q_element = std::forward<U>(element);
            state.store(STORED, R);
        }
        else {
            for(;;) {
                unsigned char expected = EMPTY;
                if(ATOMIC_QUEUE_LIKELY(state.compare_exchange_strong(expected, STORING, X, X))) {
                    q_element = std::forward<U>(element);
                    state.store(STORED, R);
                    return;
                }
                // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
                do
                    spin_loop_pause();
                while(Derived::maximize_throughput_ && state.load(X) != EMPTY);
            }
        }
    }

public:
    template<class T>
    bool try_push(T&& element) noexcept {
        auto head = head_.load(X);
        if(Derived::spsc_) {
            if(static_cast<int>(head - tail_.load(X)) >= static_cast<int>(static_cast<Derived&>(*this).size_))
                return false;
            head_.store(head + 1, X);
        }
        else {
            do {
                if(static_cast<int>(head - tail_.load(X)) >= static_cast<int>(static_cast<Derived&>(*this).size_))
                    return false;
            } while(ATOMIC_QUEUE_UNLIKELY(!head_.compare_exchange_strong(head, head + 1, A, X))); // This loop is not FIFO.
        }

        static_cast<Derived&>(*this).do_push(std::forward<T>(element), head);
        return true;
    }

    template<class T>
    bool try_pop(T& element) noexcept {
        auto tail = tail_.load(X);
        if(Derived::spsc_) {
            if(static_cast<int>(head_.load(X) - tail) <= 0)
                return false;
            tail_.store(tail + 1, X);
        }
        else {
            do {
                if(static_cast<int>(head_.load(X) - tail) <= 0)
                    return false;
            } while(ATOMIC_QUEUE_UNLIKELY(!tail_.compare_exchange_strong(tail, tail + 1, A, X))); // This loop is not FIFO.
        }

        element = static_cast<Derived&>(*this).do_pop(tail);
        return true;
    }

    template<class T>
    void push(T&& element) noexcept {
        unsigned head;
        if(Derived::spsc_) {
            head = head_.load(X);
            head_.store(head + 1, X);
        }
        else {
            constexpr auto memory_order = Derived::total_order_ ? std::memory_order_seq_cst : std::memory_order_acquire;
            head = head_.fetch_add(1, memory_order); // FIFO and total order on Intel regardless, as of 2019.
        }
        static_cast<Derived&>(*this).do_push(std::forward<T>(element), head);
    }

    auto pop() noexcept {
        unsigned tail;
        if(Derived::spsc_) {
            tail = tail_.load(X);
            tail_.store(tail + 1, X);
        }
        else {
            constexpr auto memory_order = Derived::total_order_ ? std::memory_order_seq_cst : std::memory_order_acquire;
            tail = tail_.fetch_add(1, memory_order); // FIFO and total order on Intel regardless, as of 2019.
        }
        return static_cast<Derived&>(*this).do_pop(tail);
    }

    bool was_empty() const noexcept {
        return static_cast<int>(head_.load(X) - tail_.load(X)) <= 0;
    }

    bool was_full() const noexcept {
        return static_cast<int>(head_.load(X) - tail_.load(X)) >= static_cast<int>(static_cast<Derived const&>(*this).size_);
    }

    unsigned capacity() const noexcept {
        return static_cast<Derived const&>(*this).size_;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class T, unsigned SIZE, T NIL = T{}, bool MINIMIZE_CONTENTION = true, bool MAXIMIZE_THROUGHPUT = true, bool TOTAL_ORDER = false, bool SPSC = false>
class AtomicQueue : public AtomicQueueCommon<AtomicQueue<T, SIZE, NIL, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>> {
    using Base = AtomicQueueCommon<AtomicQueue<T, SIZE, NIL, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>>;
    friend Base;

    static constexpr unsigned size_ = MINIMIZE_CONTENTION ? details::round_up_to_power_of_2(SIZE) : SIZE;
    static constexpr int SHUFFLE_BITS = details::GetIndexShuffleBits<MINIMIZE_CONTENTION, size_, CACHE_LINE_SIZE / sizeof(std::atomic<T>)>::value;
    static constexpr bool total_order_ = TOTAL_ORDER;
    static constexpr bool spsc_ = SPSC;
    static constexpr bool maximize_throughput_ = MAXIMIZE_THROUGHPUT;

    alignas(CACHE_LINE_SIZE) std::atomic<T> elements_[size_] = {}; // Empty elements are NIL.

    T do_pop(unsigned tail) noexcept {
        std::atomic<T>& q_element = details::map<SHUFFLE_BITS>(elements_, tail % size_);
        return Base::template do_pop_atomic<T, NIL>(q_element);
    }

    void do_push(T element, unsigned head) noexcept {
        std::atomic<T>& q_element = details::map<SHUFFLE_BITS>(elements_, head % size_);
        Base::template do_push_atomic<T, NIL>(element, q_element);
    }

public:
    using value_type = T;

    AtomicQueue() noexcept {
        assert(std::atomic<T>{NIL}.is_lock_free()); // This queue is for atomic elements only. AtomicQueue2 is for non-atomic ones.
        if(T{} != NIL)
            for(auto& element : elements_)
                element.store(NIL, X);
    }

    AtomicQueue(AtomicQueue const&) = delete;
    AtomicQueue& operator=(AtomicQueue const&) = delete;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class T, unsigned SIZE, bool MINIMIZE_CONTENTION = true, bool MAXIMIZE_THROUGHPUT = true, bool TOTAL_ORDER = false, bool SPSC = false>
class AtomicQueue2 : public AtomicQueueCommon<AtomicQueue2<T, SIZE, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>> {
    using Base = AtomicQueueCommon<AtomicQueue2<T, SIZE, MINIMIZE_CONTENTION, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>>;
    using State = typename Base::State;
    friend Base;

    static constexpr unsigned size_ = MINIMIZE_CONTENTION ? details::round_up_to_power_of_2(SIZE) : SIZE;
    static constexpr int SHUFFLE_BITS = details::GetIndexShuffleBits<MINIMIZE_CONTENTION, size_, CACHE_LINE_SIZE / sizeof(State)>::value;
    static constexpr bool total_order_ = TOTAL_ORDER;
    static constexpr bool spsc_ = SPSC;
    static constexpr bool maximize_throughput_ = MAXIMIZE_THROUGHPUT;

    alignas(CACHE_LINE_SIZE) std::atomic<unsigned char> states_[size_] = {};
    alignas(CACHE_LINE_SIZE) T elements_[size_] = {};

    T do_pop(unsigned tail) noexcept {
        unsigned index = details::remap_index<SHUFFLE_BITS>(tail % size_);
        return Base::template do_pop_any(states_[index], elements_[index]);
    }

    template<class U>
    void do_push(U&& element, unsigned head) noexcept {
        unsigned index = details::remap_index<SHUFFLE_BITS>(head % size_);
        Base::template do_push_any(std::forward<U>(element), states_[index], elements_[index]);
    }

public:
    using value_type = T;

    AtomicQueue2() noexcept = default;
    AtomicQueue2(AtomicQueue2 const&) = delete;
    AtomicQueue2& operator=(AtomicQueue2 const&) = delete;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class T, class A = std::allocator<T>, T NIL = T{}, bool MAXIMIZE_THROUGHPUT = true, bool TOTAL_ORDER = false, bool SPSC = false>
class AtomicQueueB : public AtomicQueueCommon<AtomicQueueB<T, A, NIL, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>>,
                     private std::allocator_traits<A>::template rebind_alloc<std::atomic<T>> {
    using Base = AtomicQueueCommon<AtomicQueueB<T, A, NIL, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>>;
    friend Base;

    static constexpr bool total_order_ = TOTAL_ORDER;
    static constexpr bool spsc_ = SPSC;
    static constexpr bool maximize_throughput_ = MAXIMIZE_THROUGHPUT;

    using AllocatorElements = typename std::allocator_traits<A>::template rebind_alloc<std::atomic<T>>;

    static constexpr auto ELEMENTS_PER_CACHE_LINE = CACHE_LINE_SIZE / sizeof(std::atomic<T>);
    static_assert(ELEMENTS_PER_CACHE_LINE, "Unexpected ELEMENTS_PER_CACHE_LINE.");

    static constexpr auto SHUFFLE_BITS = details::GetCacheLineIndexBits<ELEMENTS_PER_CACHE_LINE>::value;
    static_assert(SHUFFLE_BITS, "Unexpected SHUFFLE_BITS.");

    // AtomicQueueCommon members are stored into by readers and writers.
    // Allocate these immutable members on another cache line which never gets invalidated by stores.
    alignas(CACHE_LINE_SIZE) unsigned size_;
    std::atomic<T>* elements_;

    T do_pop(unsigned tail) noexcept {
        std::atomic<T>& q_element = details::map<SHUFFLE_BITS>(elements_, tail & (size_ - 1));
        return Base::template do_pop_atomic<T, NIL>(q_element);
    }

    void do_push(T element, unsigned head) noexcept {
        std::atomic<T>& q_element = details::map<SHUFFLE_BITS>(elements_, head & (size_ - 1));
        Base::template do_push_atomic<T, NIL>(element, q_element);
    }

public:
    using value_type = T;

    // The special member functions are not thread-safe.

    AtomicQueueB(unsigned size)
        : size_(std::max(details::round_up_to_power_of_2(size), 1u << (SHUFFLE_BITS * 2)))
        , elements_(AllocatorElements::allocate(size_)) {
        assert(std::atomic<T>{NIL}.is_lock_free()); // This queue is for atomic elements only. AtomicQueueB2 is for non-atomic ones.
        for(auto p = elements_, q = elements_ + size_; p < q; ++p)
            p->store(NIL, X);
    }

    AtomicQueueB(AtomicQueueB&& b) noexcept
        : Base(static_cast<Base&&>(b))
        , AllocatorElements(static_cast<AllocatorElements&&>(b)) // TODO: This must be noexcept, static_assert that.
        , size_(b.size_)
        , elements_(b.elements_) {
        b.size_ = 0;
        b.elements_ = 0;
    }

    AtomicQueueB& operator=(AtomicQueueB&& b) noexcept {
        b.swap(*this);
        return *this;
    }

    ~AtomicQueueB() noexcept {
        if(elements_)
            AllocatorElements::deallocate(elements_, size_); // TODO: This must be noexcept, static_assert that.
    }

    void swap(AtomicQueueB& b) noexcept {
        using std::swap;
        this->Base::swap(b);
        swap(static_cast<AllocatorElements&>(*this), static_cast<AllocatorElements&>(b));
        swap(size_, b.size_);
        swap(elements_, b.elements_);
    }

    friend void swap(AtomicQueueB& a, AtomicQueueB& b) {
        a.swap(b);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class T, class A = std::allocator<T>, bool MAXIMIZE_THROUGHPUT = true, bool TOTAL_ORDER = false, bool SPSC = false>
class AtomicQueueB2 : public AtomicQueueCommon<AtomicQueueB2<T, A, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>>,
                      private A,
                      private std::allocator_traits<A>::template rebind_alloc<std::atomic<uint8_t>> {
    using Base = AtomicQueueCommon<AtomicQueueB2<T, A, MAXIMIZE_THROUGHPUT, TOTAL_ORDER, SPSC>>;
    using State = typename Base::State;
    friend Base;

    static constexpr bool total_order_ = TOTAL_ORDER;
    static constexpr bool spsc_ = SPSC;
    static constexpr bool maximize_throughput_ = MAXIMIZE_THROUGHPUT;

    using AllocatorElements = A;
    using AllocatorStates = typename std::allocator_traits<A>::template rebind_alloc<std::atomic<uint8_t>>;

    // AtomicQueueCommon members are stored into by readers and writers.
    // Allocate these immutable members on another cache line which never gets invalidated by stores.
    alignas(CACHE_LINE_SIZE) unsigned size_;
    std::atomic<unsigned char>* states_;
    T* elements_;

    static constexpr auto STATES_PER_CACHE_LINE = CACHE_LINE_SIZE / sizeof(State);
    static_assert(STATES_PER_CACHE_LINE, "Unexpected STATES_PER_CACHE_LINE.");

    static constexpr auto SHUFFLE_BITS = details::GetCacheLineIndexBits<STATES_PER_CACHE_LINE>::value;
    static_assert(SHUFFLE_BITS, "Unexpected SHUFFLE_BITS.");

    T do_pop(unsigned tail) noexcept {
        unsigned index = details::remap_index<SHUFFLE_BITS>(tail & (size_ - 1));
        return Base::template do_pop_any(states_[index], elements_[index]);
    }

    template<class U>
    void do_push(U&& element, unsigned head) noexcept {
        unsigned index = details::remap_index<SHUFFLE_BITS>(head & (size_ - 1));
        Base::template do_push_any(std::forward<U>(element), states_[index], elements_[index]);
    }

public:
    using value_type = T;

    // The special member functions are not thread-safe.

    AtomicQueueB2(unsigned size)
        : size_(std::max(details::round_up_to_power_of_2(size), 1u << (SHUFFLE_BITS * 2)))
        , states_(AllocatorStates::allocate(size_))
        , elements_(AllocatorElements::allocate(size_)) {
        for(auto p = states_, q = states_ + size_; p < q; ++p)
            p->store(Base::EMPTY, X);

        AllocatorElements& ae = *this;
        for(auto p = elements_, q = elements_ + size_; p < q; ++p)
            std::allocator_traits<AllocatorElements>::construct(ae, p);
    }

    AtomicQueueB2(AtomicQueueB2&& b) noexcept
        : Base(static_cast<Base&&>(b))
        , AllocatorElements(static_cast<AllocatorElements&&>(b)) // TODO: This must be noexcept, static_assert that.
        , AllocatorStates(static_cast<AllocatorStates&&>(b))     // TODO: This must be noexcept, static_assert that.
        , size_(b.size_)
        , states_(b.states_)
        , elements_(b.elements_) {
        b.size_ = 0;
        b.states_ = 0;
        b.elements_ = 0;
    }

    AtomicQueueB2& operator=(AtomicQueueB2&& b) noexcept {
        b.swap(*this);
        return *this;
    }

    ~AtomicQueueB2() noexcept {
        if(elements_) {
            AllocatorElements& ae = *this;
            for(auto p = elements_, q = elements_ + size_; p < q; ++p)
                std::allocator_traits<AllocatorElements>::destroy(ae, p);
            AllocatorElements::deallocate(elements_, size_); // TODO: This must be noexcept, static_assert that.
            AllocatorStates::deallocate(states_, size_); // TODO: This must be noexcept, static_assert that.
        }
    }

    void swap(AtomicQueueB2& b) noexcept {
        using std::swap;
        this->Base::swap(b);
        swap(static_cast<AllocatorElements&>(*this), static_cast<AllocatorElements&>(b));
        swap(static_cast<AllocatorStates&>(*this), static_cast<AllocatorStates&>(b));
        swap(size_, b.size_);
        swap(states_, b.states_);
        swap(elements_, b.elements_);
    }

    friend void swap(AtomicQueueB2& a, AtomicQueueB2& b) noexcept {
        a.swap(b);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template<class Queue>
struct RetryDecorator : Queue {
    using T = typename Queue::value_type;

    using Queue::Queue;

    void push(T element) noexcept {
        while(!this->try_push(element))
            spin_loop_pause();
    }

    T pop() noexcept {
        T element;
        while(!this->try_pop(element))
            spin_loop_pause();
        return element;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace atomic_queue
} // namespace paryfor

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// paryfor parallel for implementation
// Copyright (c) 2020 Erik Garrison. MIT License
// see LICENSE for details

namespace paryfor {

using std::uint32_t;
using std::uint64_t;
using std::uint8_t;


template <typename I>
void parallel_for(const I& begin,
                  const I& end,
                  const uint64_t& nthreads,
                  const std::function<void(I, int)>& func) {
    auto queue_ptr = new atomic_queue::AtomicQueue2<I, 2 << 16>;
    auto& queue = *queue_ptr;
    std::atomic<bool> work_todo;
    auto worker =
        [&queue,&work_todo,&func](int thread_id) {
            I i;
            while (work_todo.load()) {
                if (queue.try_pop(i)) {
                    func(i, thread_id);
                } else {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
                }
            }
        };
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    work_todo.store(true);
    for (uint64_t t = 0; t < nthreads; ++t) {
        workers.emplace_back(worker, t);
    }
    I todo_i = begin;
    while (todo_i != end) {
        if (queue.try_push(todo_i)) {
            ++todo_i;
        } else {
            std::this_thread::sleep_for(std::chrono::nanoseconds(1));
        }
    }
    while (!queue.was_empty()) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1));
    }
    work_todo.store(false);
    for (uint64_t t = 0; t < nthreads; ++t) {
        workers[t].join();
    }
    delete queue_ptr;
}

// specialization where we don't use the thread id
template <typename I>
void parallel_for(const I& begin,
                  const I& end,
                  const uint64_t& nthreads,
                  const std::function<void(I)>& func) {
    parallel_for<I>(begin, end, nthreads, [&func](I i, int id) { func(i); });
}

template <typename I>
void parallel_for(const I& begin,
                  const I& end,
                  const uint64_t& nthreads,
                  const uint64_t& chunk_size,
                  const std::function<void(I, int)>& func) {
    auto queue_ptr = new atomic_queue::AtomicQueue2<std::pair<I, I>, 2 << 16>;
    auto& queue = *queue_ptr;
    std::atomic<bool> work_todo;
    auto worker =
        [&queue,&work_todo,&func](int thread_id) {
            std::pair<I, I> p;
            while (work_todo.load()) {
                if (queue.try_pop(p)) {
                    for (I i = p.first; i < p.second; ++i) {
                        func(i, thread_id);
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
                }
            }
        };
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    work_todo.store(true);
    for (uint64_t t = 0; t < nthreads; ++t) {
        workers.emplace_back(worker, t);
    }
    std::pair<I, I> todo_range = std::make_pair(begin, std::min(begin + static_cast<I>(chunk_size), end));
    I& todo_i = todo_range.first;
    I& todo_j = todo_range.second;
    while (todo_i != end) {
        if (queue.try_push(todo_range)) {
            todo_i = std::min(todo_i + static_cast<I>(chunk_size), end);
            todo_j = std::min(todo_j + static_cast<I>(chunk_size), end);
        } else {
            std::this_thread::sleep_for(std::chrono::nanoseconds(1));
        }
    }
    while (!queue.was_empty()) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1));
    }
    work_todo.store(false);
    for (uint64_t t = 0; t < nthreads; ++t) {
        workers[t].join();
    }
    delete queue_ptr;
}

// specialization where we don't use the thread id
template <typename I>
void parallel_for(const I& begin,
                  const I& end,
                  const uint64_t& nthreads,
                  const uint64_t& chunk_size,
                  const std::function<void(I)>& func) {
    parallel_for<I>(begin, end, nthreads, chunk_size, [&func](I i, int id) { func(i); });
}

}
