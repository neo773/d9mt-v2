#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace dxvk {

template<typename Allocator, bool Stateless>
struct SmallVectorState {
  SmallVectorState(size_t initialCapacity, const Allocator& alloc)
  : capacity(initialCapacity), allocator(alloc) { }

  size_t capacity = 0;
  size_t size     = 0;

  Allocator allocator = { };

  Allocator& getAllocator() {
    return allocator;
  }

  void setAllocator(const Allocator& alloc) {
    allocator = alloc;
  }
};


template<typename Allocator>
struct SmallVectorState<Allocator, true> {
  SmallVectorState(size_t initialCapacity, const Allocator&)
  : capacity(initialCapacity) { }

  size_t capacity = 0;
  size_t size     = 0;

  Allocator getAllocator() {
    return Allocator();
  }

  void setAllocator(const Allocator&) { }
};


template<typename T, size_t N, typename Allocator = std::allocator<T>>
class small_vector {
  using storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

  template<typename T_, size_t N_, typename Allocator_>
  friend class small_vector;
public:

  constexpr static size_t EmbeddedCapacity = N;

  using iterator = T*;
  using const_iterator = const T*;

  small_vector()
  : small_vector(Allocator()) { }

  explicit small_vector(const Allocator& alloc)
  : m_state(N, alloc) {
    u.m_ptr = nullptr;
  }

  explicit small_vector(size_t size, T value = T(), const Allocator& alloc = Allocator())
  : small_vector(alloc) {
    resize(size, std::move(value));
  }

  small_vector(const std::initializer_list<T>& list, const Allocator& alloc = Allocator())
  : small_vector(alloc) {
    reserve(list.size());

    for (auto iter = list.begin(); iter != list.end(); iter++)
      new (ptr(m_state.size++)) T(*iter);
  }

  small_vector(small_vector&& other)
  : m_state(N, other.m_state.getAllocator()) {
    move(std::move(other));
  }

  template<size_t N_, typename Allocator_>
  small_vector(small_vector<T, N_, Allocator_>&& other)
  : m_state(N, other.m_state.getAllocator()) {
    move(std::move(other));
  }

  small_vector(const small_vector& other)
  : m_state(N, Allocator()) {
    copy(other);
  }

  template<size_t N_, typename Allocator_>
  small_vector(const small_vector<T, N_, Allocator_>& other)
  : m_state(N, Allocator()) {
    copy(other);
  }

  small_vector& operator = (small_vector&& other) {
    free();
    move(std::move(other));
    return *this;
  }

  template<size_t N_, typename Allocator_>
  small_vector& operator = (small_vector<T, N_, Allocator_>&& other) {
    free();
    move(std::move(other));
    return *this;
  }

  small_vector& operator = (const small_vector& other) {
    if (this == &other)
      return *this;

    clear();
    copy(other);
    return *this;
  }

  template<size_t N_, typename Allocator_>
  small_vector& operator = (const small_vector<T, N_, Allocator_>& other) {
    clear();
    copy(other);
    return *this;
  }

  ~small_vector() {
    free();
  }

  const T* data() const { return ptr(0); }
        T* data()       { return ptr(0); }

  size_t size() const {
    return m_state.size;
  }

  bool empty() const {
    return !size();
  }

  size_t capacity() const {
    return m_state.capacity;
  }

  bool is_embedded() const {
    return capacity() <= EmbeddedCapacity;
  }

  void reserve(size_t n) {
    n = pick_capacity(n);

    if (n <= capacity())
      return;

    T* data = m_state.getAllocator().allocate(n);

    for (size_t i = 0; i < size(); i++) {
      auto object = std::launder(ptr(i));

      new (&data[i]) T(std::move(*object));
      object->~T();
    }

    if (!is_embedded())
      m_state.getAllocator().deallocate(u.m_ptr, capacity());

    m_state.capacity = n;
    u.m_ptr = data;
  }

  void resize(size_t n, T value = T()) {
    reserve(n);

    for (size_t i = n; i < size(); i++)
      std::launder(ptr(i))->~T();

    for (size_t i = size(); i < n; i++)
      new (ptr(i)) T(value);

    m_state.size = n;
  }

  void push_back(const T& object) {
    reserve(m_state.size + 1);
    new (ptr(m_state.size++)) T(object);
  }

  void push_back(T&& object) {
    reserve(m_state.size + 1);
    new (ptr(m_state.size++)) T(std::move(object));
  }

  template<typename... Args>
  T& emplace_back(Args&&... args) {
    reserve(m_state.size + 1);
    return *(new (ptr(m_state.size++)) T(std::forward<Args>(args)...));
  }

  iterator erase(size_t idx) {
    std::launder(ptr(idx))->~T();

    for (size_t i = idx; i < size() - 1; i++) {
      auto object = std::launder(ptr(i + 1));

      new (ptr(i)) T(std::move(*object));
      object->~T();
    }

    m_state.size--;
    return begin() + idx;
  }

  iterator erase(const T* iter) {
    return erase(std::distance(cbegin(), iter));
  }

  iterator insert(const_iterator iter, const T& element) {
    auto ptr = insert(std::distance(cbegin(), iter));
    return new (ptr) T(element);
  }

  iterator insert(const_iterator iter, T&& element) {
    auto ptr = insert(std::distance(cbegin(), iter));
    return new (ptr) T(std::move(element));
  }

  void pop_back() {
    std::launder(ptr(--m_state.size))->~T();
  }

  void clear() {
    for (size_t i = 1; i <= size(); i++)
      std::launder(ptr(size() - i))->~T();

    m_state.size = 0;
  }

  void shrink_to_fit() {
    if (is_embedded() || size() == capacity())
      return;

    auto oldCapacity = capacity();
    auto oldStorage = u.m_ptr;

    m_state.capacity = std::max(size(), EmbeddedCapacity);

    if (!is_embedded())
      u.m_ptr = m_state.getAllocator().allocate(capacity());

    for (size_t i = 0u; i < size(); i++) {
      new (ptr(i)) T(std::move(oldStorage[i]));
      oldStorage[i].~T();
    }

    m_state.getAllocator().deallocate(oldStorage, oldCapacity);
  }

  const T& operator [] (size_t idx) const { return *ptr(idx); }
        T& operator [] (size_t idx)       { return *ptr(idx); }

  T* begin() { return ptr(0); }
  T* end() { return begin() + size(); }

  const T* begin() const { return ptr(0); }
  const T* end() const { return begin() + size(); }

  const T* cbegin() const { return begin(); }
  const T* cend() const { return end(); }

  const T& front() const { return *ptr(0); }
        T& front()       { return *ptr(0); }

  const T& back() const { return *ptr(size() - 1); }
        T& back()       { return *ptr(size() - 1); }

private:

  SmallVectorState<Allocator, std::allocator_traits<Allocator>::is_always_equal::value> m_state;

  union {
    T*      m_ptr;
    storage m_data[N];
  } u;

  size_t pick_capacity(size_t n) {
    size_t newCapacity = capacity();

    while (newCapacity < n)
      newCapacity *= 2;

    return newCapacity;
  }

  T* ptr(size_t idx) {
    return is_embedded()
      ? reinterpret_cast<T*>(u.m_data + idx)
      : u.m_ptr + idx;
  }

  const T* ptr(size_t idx) const {
    return is_embedded()
      ? reinterpret_cast<const T*>(u.m_data + idx)
      : u.m_ptr + idx;
  }

  void free() {
    for (size_t i = 0; i < size(); i++)
      std::launder(ptr(i))->~T();

    if (!is_embedded())
      m_state.getAllocator().deallocate(u.m_ptr, capacity());

    m_state.capacity = N;
    m_state.size = 0;
  }

  template<size_t N_, typename Allocator_>
  void move(small_vector<T, N_, Allocator_>&& other) {
    if constexpr (std::is_same_v<Allocator, Allocator_>)
      m_state.setAllocator(other.m_state.getAllocator());

    if (!std::is_same_v<Allocator, Allocator_> || other.capacity() <= std::max(N, N_)) {
      reserve(other.size());

      for (size_t i = 0; i < other.size(); i++) {
        auto object = std::launder(other.ptr(i));

        new (ptr(i)) T(std::move(*object));
        object->~T();
      }

      m_state.size = std::exchange(other.m_state.size, size_t(0));
    } else {
      m_state.capacity = std::exchange(other.m_state.capacity, N_);
      m_state.size = std::exchange(other.m_state.size, size_t(0));
      u.m_ptr = std::exchange(other.u.m_ptr, nullptr);
    }
  }

  template<size_t N_, typename Allocator_>
  void copy(const small_vector<T, N_, Allocator_>& other) {
    reserve(other.size());

    for (size_t i = 0; i < other.size(); i++)
      emplace_back(other[i]);
  }

  T* insert(size_t idx) {
    size_t last = size();
    reserve(last + 1u);

    while (last > idx) {
      auto prev = std::launder(ptr(last - 1u));
      new (ptr(last--)) T(std::move(*prev));
      prev->~T();
    }

    m_state.size++;
    return ptr(idx);
  }

};

}
