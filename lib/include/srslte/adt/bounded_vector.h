/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#ifndef SRSLTE_BOUNDED_VECTOR_H
#define SRSLTE_BOUNDED_VECTOR_H

#include <cassert>
#include <iterator>
#include <memory>
#include <type_traits>

namespace srslte {

template <typename T, std::size_t MAX_N>
class bounded_vector
{
public:
  using iterator       = T*;
  using const_iterator = const T*;
  using size_type      = std::size_t;
  using value_type     = T;

  bounded_vector() = default;
  explicit bounded_vector(size_type N) { append(N); }
  bounded_vector(size_type N, const T& val) { append(N, val); }
  bounded_vector(const bounded_vector& other) { append(other.begin(), other.end()); }
  bounded_vector(bounded_vector&& other) noexcept
  {
    static_assert(std::is_move_constructible<T>::value, "T must be move-constructible");
    std::uninitialized_copy(std::make_move_iterator(other.begin()), std::make_move_iterator(other.end()), end());
    size_ = other.size();
    other.clear();
  }
  bounded_vector(std::initializer_list<T> init) { append(init.begin(), init.end()); }
  bounded_vector(const_iterator it_begin, const_iterator it_end) { append(it_begin, it_end); }
  ~bounded_vector() { destroy(begin(), end()); }
  bounded_vector& operator=(const bounded_vector& other)
  {
    if (this == &other) {
      return *this;
    }
    assign(other.begin(), other.end());
    return *this;
  }
  bounded_vector& operator=(bounded_vector&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    size_t min_common_size = std::min(other.size(), size());
    if (min_common_size > 0) {
      // move already constructed elements
      auto it = std::move(other.begin(), other.begin() + min_common_size, begin());
      destroy(it, end());
    } else {
      clear();
    }
    // append the rest
    std::uninitialized_copy(
        std::make_move_iterator(other.begin() + min_common_size), std::make_move_iterator(other.end()), end());
    size_ = other.size();
    other.clear();
    return *this;
  }

  void assign(size_type nof_elems, const T& value)
  {
    clear();
    append(nof_elems, value);
  }
  void assign(const_iterator it_start, const_iterator it_end)
  {
    clear();
    append(it_start, it_end);
  }
  void assign(std::initializer_list<T> ilist) { assign(ilist.begin(), ilist.end()); }

  // Element access
  T& operator[](std::size_t i)
  {
    assert(i < size_ && "Array index is out of bounds.");
    return reinterpret_cast<T&>(buffer[i]);
  }
  const T& operator[](std::size_t i) const
  {
    assert(i < size_ && "Array index is out of bounds.");
    return reinterpret_cast<const T&>(buffer[i]);
  }
  T& back()
  {
    assert(size_ > 0 && "Trying to get back of empty array.");
    return *(begin() + size_ - 1);
  }
  const T& back() const
  {
    assert(size_ > 0 && "Trying to get back of empty array.");
    return *(begin() + size_ - 1);
  }
  T&       front() { return (*this)[0]; }
  const T& front() const { return (*this)[0]; }
  T*       data() { return &front(); }
  const T* data() const { return &front(); }

  // Iterators
  iterator       begin() { return data(); }
  iterator       end() { return begin() + size_; }
  const_iterator begin() const { return data(); }
  const_iterator end() const { return begin() + size_; }

  // Capacity
  bool        empty() const { return size_ == 0; }
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return MAX_N; }
  bool        full() const { return size_ == MAX_N; }

  // modifiers
  void clear()
  {
    destroy(begin(), end());
    size_ = 0;
  }
  iterator erase(iterator pos)
  {
    assert(pos >= this->begin() && "Iterator to erase is out of bounds.");
    assert(pos < this->end() && "Erasing at past-the-end iterator.");
    iterator ret = pos;
    std::move(pos + 1, end(), pos);
    pop_back();
    return ret;
  }
  iterator erase(iterator it_start, iterator it_end)
  {
    assert(it_start >= begin() && "Range to erase is out of bounds.");
    assert(it_start <= it_end && "Trying to erase invalid range.");
    assert(it_end <= end() && "Trying to erase past the end.");

    iterator ret = it_start;
    // Shift all elts down.
    iterator new_end = std::move(it_end, end(), it_start);
    destroy(new_end, end());
    size_ = new_end - begin();
    return ret;
  }
  void push_back(const T& value)
  {
    static_assert(std::is_copy_constructible<T>::value, "T must be copy-constructible");
    size_++;
    assert(size_ <= MAX_N);
    new (&back()) T(value);
  }
  void push_back(T&& value)
  {
    static_assert(std::is_move_constructible<T>::value, "T must be move-constructible");
    size_++;
    assert(size_ <= MAX_N);
    new (&back()) T(std::move(value));
  }
  template <typename... Args>
  void emplace_back(Args&&... args)
  {
    static_assert(std::is_constructible<T, Args&&...>::value, "Passed arguments to emplace_back are invalid");
    size_++;
    assert(size_ <= MAX_N);
    new (&back()) T(std::forward<Args>(args)...);
  }
  void pop_back()
  {
    assert(size_ > 0 && "Trying to erase element from empty vector.");
    back().~T();
    size_--;
  }
  void resize(size_type count)
  {
    static_assert(std::is_default_constructible<T>::value, "T must be default constructible");
    resize(count, T());
  }
  void resize(size_type count, const T& value)
  {
    static_assert(std::is_copy_constructible<T>::value, "T must be copy constructible");
    if (size_ > count) {
      destroy(begin() + count, end());
      size_ = count;
    } else if (size_ < count) {
      append(count - size_, value);
    }
  }

  bool operator==(const bounded_vector& other) const
  {
    return other.size() == size() and std::equal(begin(), end(), other.begin());
  }
  bool operator!=(const bounded_vector& other) const { return not(*this == other); }

private:
  void destroy(iterator it_start, iterator it_end)
  {
    for (auto it = it_start; it != it_end; ++it) {
      it->~T();
    }
  }
  void append(const_iterator it_begin, const_iterator it_end)
  {
    size_type N = std::distance(it_begin, it_end);
    assert(N + size_ <= MAX_N);
    std::uninitialized_copy(it_begin, it_end, end());
    size_ += N;
  }
  void append(size_type N, const T& element)
  {
    static_assert(std::is_copy_constructible<T>::value, "T must be copy-constructible");
    assert(N + size_ <= MAX_N);
    std::uninitialized_fill_n(end(), N, element);
    size_ += N;
  }
  void append(size_type N)
  {
    static_assert(std::is_default_constructible<T>::value, "T must be default-constructible");
    assert(N + size_ <= MAX_N);
    for (size_type i = size_; i < size_ + N; ++i) {
      new (&buffer[i]) T();
    }
    size_ += N;
  }

  std::size_t                                                size_ = 0;
  typename std::aligned_storage<sizeof(T), alignof(T)>::type buffer[MAX_N];
};
} // namespace srslte

#endif // SRSLTE_BOUNDED_VECTOR_H
