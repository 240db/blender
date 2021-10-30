/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

/** \file
 * \ingroup fn
 *
 * A generic virtual array is the same as a virtual array from blenlib, except for the fact that
 * the data type is only known at runtime.
 */

#include <optional>

#include "BLI_virtual_array.hh"

#include "FN_generic_array.hh"
#include "FN_generic_span.hh"

namespace blender::fn {

/* -------------------------------------------------------------------- */
/** \name #GVArrayImpl and #GVMutableArrayImpl.
 * \{ */

class GVArray;
class GVArrayImpl;
class GVMutableArray;
class GVMutableArrayImpl;

/* A generically typed version of `VArrayImpl<T>`. */
class GVArrayImpl {
 protected:
  const CPPType *type_;
  int64_t size_;

 public:
  GVArrayImpl(const CPPType &type, const int64_t size);
  virtual ~GVArrayImpl() = default;

  const CPPType &type() const;

  int64_t size() const;
  bool is_empty() const;

  void get(const int64_t index, void *r_value) const;
  void get_to_uninitialized(const int64_t index, void *r_value) const;

  bool is_span() const;
  GSpan get_internal_span() const;

  bool is_single() const;
  void get_internal_single(void *r_value) const;
  void get_internal_single_to_uninitialized(void *r_value) const;

  void materialize(void *dst) const;
  void materialize(const IndexMask mask, void *dst) const;

  void materialize_to_uninitialized(void *dst) const;
  void materialize_to_uninitialized(const IndexMask mask, void *dst) const;

  template<typename T> bool try_assign_VArray(VArray<T> &varray) const;
  bool has_ownership() const;

 protected:
  virtual void get_impl(const int64_t index, void *r_value) const;
  virtual void get_to_uninitialized_impl(const int64_t index, void *r_value) const = 0;

  virtual bool is_span_impl() const;
  virtual GSpan get_internal_span_impl() const;

  virtual bool is_single_impl() const;
  virtual void get_internal_single_impl(void *UNUSED(r_value)) const;

  virtual void materialize_impl(const IndexMask mask, void *dst) const;
  virtual void materialize_to_uninitialized_impl(const IndexMask mask, void *dst) const;

  virtual bool try_assign_VArray_impl(void *varray) const;
  virtual bool has_ownership_impl() const;
};

/* Similar to GVArrayImpl, but supports changing the elements in the virtual array. */
class GVMutableArrayImpl : public GVArrayImpl {
 public:
  GVMutableArrayImpl(const CPPType &type, const int64_t size);

  void set_by_copy(const int64_t index, const void *value);
  void set_by_move(const int64_t index, void *value);
  void set_by_relocate(const int64_t index, void *value);

  void fill(const void *value);
  void set_all(const void *src);

  GMutableSpan get_internal_span();

  template<typename T> bool try_assign_VMutableArray(VMutableArray<T> &varray) const;

 protected:
  virtual void set_by_copy_impl(const int64_t index, const void *value);
  virtual void set_by_relocate_impl(const int64_t index, void *value);
  virtual void set_by_move_impl(const int64_t index, void *value) = 0;

  virtual void set_all_impl(const void *src);

  virtual bool try_assign_VMutableArray_impl(void *varray) const;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #GVArray and #GVMutableArray
 * \{ */

namespace detail {
struct GVArrayAnyExtraInfo {
  const GVArrayImpl *(*get_varray)(const void *buffer) =
      [](const void *UNUSED(buffer)) -> const GVArrayImpl * { return nullptr; };

  template<typename StorageT> static GVArrayAnyExtraInfo get();
};
}  // namespace detail

class GVMutableArray;

class GVArrayCommon {
 protected:
  using ExtraInfo = detail::GVArrayAnyExtraInfo;
  using Storage = Any<ExtraInfo, 32, 8>;

  const GVArrayImpl *impl_ = nullptr;
  Storage storage_;

 protected:
  GVArrayCommon() = default;

  GVArrayCommon(const GVArrayCommon &other) : storage_(other.storage_)
  {
    impl_ = this->impl_from_storage();
  }

  GVArrayCommon(GVArrayCommon &&other) : storage_(std::move(other.storage_))
  {
    impl_ = this->impl_from_storage();
    other.storage_.reset();
    other.impl_ = nullptr;
  }

  GVArrayCommon(const GVArrayImpl *impl) : impl_(impl)
  {
    storage_ = impl_;
  }

  GVArrayCommon(std::shared_ptr<const GVArrayImpl> impl) : impl_(impl.get())
  {
    if (impl) {
      storage_ = std::move(impl);
    }
  }

  template<typename ImplT, typename... Args> void emplace(Args &&...args)
  {
    static_assert(std::is_base_of_v<GVArrayImpl, ImplT>);
    if constexpr (std::is_copy_constructible_v<ImplT> && Storage::template is_inline_v<ImplT>) {
      impl_ = &storage_.template emplace<ImplT>(std::forward<Args>(args)...);
    }
    else {
      std::shared_ptr<const GVArrayImpl> ptr = std::make_shared<ImplT>(
          std::forward<Args>(args)...);
      impl_ = &*ptr;
      storage_ = std::move(ptr);
    }
  }

  void copy_from(const GVArrayCommon &other)
  {
    if (this == &other) {
      return;
    }
    storage_ = other.storage_;
    impl_ = this->impl_from_storage();
  }

  void move_from(GVArrayCommon &&other)
  {
    if (this == &other) {
      return;
    }
    storage_ = std::move(other.storage_);
    impl_ = this->impl_from_storage();
    other.storage_.reset();
    other.impl_ = nullptr;
  }

  const GVArrayImpl *impl_from_storage() const
  {
    return storage_.extra_info().get_varray(storage_.get());
  }

 public:
  operator bool() const
  {
    return impl_ != nullptr;
  }
};

class GVArray : public GVArrayCommon {
 private:
  friend GVMutableArray;

 public:
  GVArray() = default;

  GVArray(const GVArray &other);
  GVArray(GVArray &&other);
  GVArray(const GVArrayImpl *impl);
  GVArray(std::shared_ptr<const GVArrayImpl> impl);

  template<typename T> GVArray(const VArray<T> &varray);
  template<typename T> VArray<T> typed() const;

  template<typename ImplT, typename... Args> static GVArray For(Args &&...args);

  static GVArray ForSingle(const CPPType &type, const int64_t size, const void *value);
  static GVArray ForSingleRef(const CPPType &type, const int64_t size, const void *value);
  static GVArray ForSingleDefault(const CPPType &type, const int64_t size);
  static GVArray ForSpan(GSpan span);
  static GVArray ForGArray(GArray<> array);
  static GVArray ForEmpty(const CPPType &type);

  GVArray slice(IndexRange slice) const;

  GVArray &operator=(const GVArray &other);
  GVArray &operator=(GVArray &&other);

  const GVArrayImpl *operator->() const;
  const GVArrayImpl &operator*() const;
};

class GVMutableArray : public GVArrayCommon {
 public:
  GVMutableArray() = default;
  GVMutableArray(const GVMutableArray &other);
  GVMutableArray(GVMutableArray &&other);
  GVMutableArray(GVMutableArrayImpl *impl);
  GVMutableArray(std::shared_ptr<GVMutableArrayImpl> impl);

  template<typename T> GVMutableArray(const VMutableArray<T> &varray);
  template<typename T> VMutableArray<T> typed() const;

  template<typename ImplT, typename... Args> static GVMutableArray For(Args &&...args);

  static GVMutableArray ForSpan(GMutableSpan span);

  operator GVArray() const;

  GVMutableArray &operator=(const GVMutableArray &other);
  GVMutableArray &operator=(GVMutableArray &&other);

  GVMutableArrayImpl *operator->() const;
  GVMutableArrayImpl &operator*() const;

 private:
  GVMutableArrayImpl *get_impl() const
  {
    return (GVMutableArrayImpl *)impl_;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #GVArray_GSpan and #GVMutableArray_GSpan.
 * \{ */

/* A generic version of VArray_Span. */
class GVArray_GSpan : public GSpan {
 private:
  GVArray varray_;
  void *owned_data_ = nullptr;

 public:
  GVArray_GSpan(GVArray varray);
  ~GVArray_GSpan();
};

/* A generic version of VMutableArray_Span. */
class GVMutableArray_GSpan : public GMutableSpan {
 private:
  GVMutableArray varray_;
  void *owned_data_ = nullptr;
  bool save_has_been_called_ = false;
  bool show_not_saved_warning_ = true;

 public:
  GVMutableArray_GSpan(GVMutableArray varray, bool copy_values_to_span = true);
  ~GVMutableArray_GSpan();

  void save();
  void disable_not_applied_warning();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversions between generic and typed virtual arrays.
 * \{ */

/* Used to convert a typed virtual array into a generic one. */
template<typename T> class GVArrayImpl_For_VArray : public GVArrayImpl {
 protected:
  VArray<T> varray_;

 public:
  GVArrayImpl_For_VArray(VArray<T> varray)
      : GVArrayImpl(CPPType::get<T>(), varray.size()), varray_(std::move(varray))
  {
  }

 protected:
  void get_impl(const int64_t index, void *r_value) const override
  {
    *(T *)r_value = varray_[index];
  }

  void get_to_uninitialized_impl(const int64_t index, void *r_value) const override
  {
    new (r_value) T(varray_[index]);
  }

  bool is_span_impl() const override
  {
    return varray_.is_span();
  }

  GSpan get_internal_span_impl() const override
  {
    return GSpan(varray_.get_internal_span());
  }

  bool is_single_impl() const override
  {
    return varray_.is_single();
  }

  void get_internal_single_impl(void *r_value) const override
  {
    *(T *)r_value = varray_.get_internal_single();
  }

  void materialize_impl(const IndexMask mask, void *dst) const override
  {
    varray_.materialize(mask, MutableSpan((T *)dst, mask.min_array_size()));
  }

  void materialize_to_uninitialized_impl(const IndexMask mask, void *dst) const override
  {
    varray_.materialize_to_uninitialized(mask, MutableSpan((T *)dst, mask.min_array_size()));
  }

  bool try_assign_VArray_impl(void *varray) const override
  {
    *(VArray<T> *)varray = varray_;
    return true;
  }

  bool has_ownership_impl() const
  {
    return varray_.has_ownership();
  }
};

/* Used to convert any generic virtual array into a typed one. */
template<typename T> class VArrayImpl_For_GVArray : public VArrayImpl<T> {
 protected:
  GVArray varray_;

 public:
  VArrayImpl_For_GVArray(GVArray varray)
      : VArrayImpl<T>(varray->size()), varray_(std::move(varray))
  {
    BLI_assert(varray_);
    BLI_assert(varray_->type().is<T>());
  }

 protected:
  T get_impl(const int64_t index) const override
  {
    T value;
    varray_->get(index, &value);
    return value;
  }

  bool is_span_impl() const override
  {
    return varray_->is_span();
  }

  Span<T> get_internal_span_impl() const override
  {
    return varray_->get_internal_span().template typed<T>();
  }

  bool is_single_impl() const override
  {
    return varray_->is_single();
  }

  T get_internal_single_impl() const override
  {
    T value;
    varray_->get_internal_single(&value);
    return value;
  }

  bool try_assign_GVArray_impl(GVArray &varray) const override
  {
    varray = varray_;
    return true;
  }

  bool has_ownership_impl() const override
  {
    return varray_->has_ownership();
  }
};

/* Used to convert any typed virtual mutable array into a generic one. */
template<typename T> class GVMutableArrayImpl_For_VMutableArray : public GVMutableArrayImpl {
 protected:
  VMutableArray<T> varray_;

 public:
  GVMutableArrayImpl_For_VMutableArray(VMutableArray<T> varray)
      : GVMutableArrayImpl(CPPType::get<T>(), varray->size()), varray_(std::move(varray))
  {
  }

 protected:
  void get_impl(const int64_t index, void *r_value) const override
  {
    *(T *)r_value = varray_[index];
  }

  void get_to_uninitialized_impl(const int64_t index, void *r_value) const override
  {
    new (r_value) T(varray_[index]);
  }

  bool is_span_impl() const override
  {
    return varray_.is_span();
  }

  GSpan get_internal_span_impl() const override
  {
    Span<T> span = varray_.get_internal_span();
    return span;
  }

  bool is_single_impl() const override
  {
    return varray_.is_single();
  }

  void get_internal_single_impl(void *r_value) const override
  {
    *(T *)r_value = varray_.get_internal_single();
  }

  void set_by_copy_impl(const int64_t index, const void *value) override
  {
    const T &value_ = *(const T *)value;
    varray_.set(index, value_);
  }

  void set_by_relocate_impl(const int64_t index, void *value) override
  {
    T &value_ = *(T *)value;
    varray_.set(index, std::move(value_));
    value_.~T();
  }

  void set_by_move_impl(const int64_t index, void *value) override
  {
    T &value_ = *(T *)value;
    varray_.set(index, std::move(value_));
  }

  void set_all_impl(const void *src) override
  {
    varray_.set_all(Span((T *)src, size_));
  }

  void materialize_impl(const IndexMask mask, void *dst) const override
  {
    varray_.materialize(mask, MutableSpan((T *)dst, mask.min_array_size()));
  }

  void materialize_to_uninitialized_impl(const IndexMask mask, void *dst) const override
  {
    varray_.materialize_to_uninitialized(mask, MutableSpan((T *)dst, mask.min_array_size()));
  }

  bool try_assign_VArray_impl(void *varray) const override
  {
    *(VArray<T> *)varray = varray_;
    return true;
  }

  bool try_assign_VMutableArray_impl(void *varray) const override
  {
    *(VMutableArray<T> *)varray = varray_;
    return true;
  }

  bool has_ownership_impl() const
  {
    return varray_.has_ownership();
  }
};

/* Used to convert an generic mutable virtual array into a typed one. */
template<typename T> class VMutableArrayImpl_For_GVMutableArray : public VMutableArrayImpl<T> {
 protected:
  GVMutableArray varray_;

 public:
  VMutableArrayImpl_For_GVMutableArray(GVMutableArray varray)
      : VMutableArrayImpl<T>(varray->size()), varray_(varray)
  {
    BLI_assert(varray_);
    BLI_assert(varray_->type().is<T>());
  }

 private:
  T get_impl(const int64_t index) const override
  {
    T value;
    varray_->get(index, &value);
    return value;
  }

  void set_impl(const int64_t index, T value) override
  {
    varray_->set_by_relocate(index, &value);
  }

  bool is_span_impl() const override
  {
    return varray_->is_span();
  }

  Span<T> get_internal_span_impl() const override
  {
    return varray_->get_internal_span().template typed<T>();
  }

  bool is_single_impl() const override
  {
    return varray_->is_single();
  }

  T get_internal_single_impl() const override
  {
    T value;
    varray_->get_internal_single(&value);
    return value;
  }

  bool try_assign_GVArray_impl(GVArray &varray) const override
  {
    varray = varray_;
    return true;
  }

  bool try_assign_GVMutableArray_impl(GVMutableArray &varray) const override
  {
    varray = varray_;
    return true;
  }

  bool has_ownership_impl() const override
  {
    return varray_->has_ownership();
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #GVArrayImpl_For_GSpan and #GVMutableArrayImpl_For_GMutableSpan.
 * \{ */

class GVArrayImpl_For_GSpan : public GVArrayImpl {
 protected:
  const void *data_ = nullptr;
  const int64_t element_size_;

 public:
  GVArrayImpl_For_GSpan(const GSpan span);

 protected:
  GVArrayImpl_For_GSpan(const CPPType &type, const int64_t size);

  void get_impl(const int64_t index, void *r_value) const override;
  void get_to_uninitialized_impl(const int64_t index, void *r_value) const override;

  bool is_span_impl() const override;
  GSpan get_internal_span_impl() const override;
};

class GVMutableArrayImpl_For_GMutableSpan : public GVMutableArrayImpl {
 protected:
  void *data_ = nullptr;
  const int64_t element_size_;

 public:
  GVMutableArrayImpl_For_GMutableSpan(const GMutableSpan span);

 protected:
  GVMutableArrayImpl_For_GMutableSpan(const CPPType &type, const int64_t size);

  void get_impl(const int64_t index, void *r_value) const override;
  void get_to_uninitialized_impl(const int64_t index, void *r_value) const override;

  void set_by_copy_impl(const int64_t index, const void *value) override;
  void set_by_move_impl(const int64_t index, void *value) override;
  void set_by_relocate_impl(const int64_t index, void *value) override;

  bool is_span_impl() const override;
  GSpan get_internal_span_impl() const override;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Inline methods for #GVArrayImpl.
 * \{ */

inline GVArrayImpl::GVArrayImpl(const CPPType &type, const int64_t size)
    : type_(&type), size_(size)
{
  BLI_assert(size_ >= 0);
}

inline const CPPType &GVArrayImpl::type() const
{
  return *type_;
}

inline int64_t GVArrayImpl::size() const
{
  return size_;
}

inline bool GVArrayImpl::is_empty() const
{
  return size_ == 0;
}

/* Copies the value at the given index into the provided storage. The `r_value` pointer is
 * expected to point to initialized memory. */
inline void GVArrayImpl::get(const int64_t index, void *r_value) const
{
  BLI_assert(index >= 0);
  BLI_assert(index < size_);
  this->get_impl(index, r_value);
}

/* Same as `get`, but `r_value` is expected to point to uninitialized memory. */
inline void GVArrayImpl::get_to_uninitialized(const int64_t index, void *r_value) const
{
  BLI_assert(index >= 0);
  BLI_assert(index < size_);
  this->get_to_uninitialized_impl(index, r_value);
}

/* Returns true when the virtual array is stored as a span internally. */
inline bool GVArrayImpl::is_span() const
{
  if (size_ == 0) {
    return true;
  }
  return this->is_span_impl();
}

/* Returns the internally used span of the virtual array. This invokes undefined behavior is the
 * virtual array is not stored as a span internally. */
inline GSpan GVArrayImpl::get_internal_span() const
{
  BLI_assert(this->is_span());
  if (size_ == 0) {
    return GSpan(*type_);
  }
  return this->get_internal_span_impl();
}

/* Returns true when the virtual array returns the same value for every index. */
inline bool GVArrayImpl::is_single() const
{
  if (size_ == 1) {
    return true;
  }
  return this->is_single_impl();
}

/* Copies the value that is used for every element into `r_value`, which is expected to point to
 * initialized memory. This invokes undefined behavior if the virtual array would not return the
 * same value for every index. */
inline void GVArrayImpl::get_internal_single(void *r_value) const
{
  BLI_assert(this->is_single());
  if (size_ == 1) {
    this->get(0, r_value);
    return;
  }
  this->get_internal_single_impl(r_value);
}

/* Same as `get_internal_single`, but `r_value` points to initialized memory. */
inline void GVArrayImpl::get_internal_single_to_uninitialized(void *r_value) const
{
  type_->default_construct(r_value);
  this->get_internal_single(r_value);
}

template<typename T> inline bool GVArrayImpl::try_assign_VArray(VArray<T> &varray) const
{
  BLI_assert(type_->is<T>());
  return this->try_assign_VArray_impl(&varray);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Inline methods for #GVMutableArrayImpl.
 * \{ */

inline GVMutableArrayImpl::GVMutableArrayImpl(const CPPType &type, const int64_t size)
    : GVArrayImpl(type, size)
{
}

inline void GVMutableArrayImpl::set_by_copy(const int64_t index, const void *value)
{
  BLI_assert(index >= 0);
  BLI_assert(index < size_);
  this->set_by_copy_impl(index, value);
}

inline void GVMutableArrayImpl::set_by_move(const int64_t index, void *value)
{
  BLI_assert(index >= 0);
  BLI_assert(index < size_);
  this->set_by_move_impl(index, value);
}

inline void GVMutableArrayImpl::set_by_relocate(const int64_t index, void *value)
{
  BLI_assert(index >= 0);
  BLI_assert(index < size_);
  this->set_by_relocate_impl(index, value);
}

inline GMutableSpan GVMutableArrayImpl::get_internal_span()
{
  BLI_assert(this->is_span());
  GSpan span = static_cast<const GVArrayImpl *>(this)->get_internal_span();
  return GMutableSpan(span.type(), const_cast<void *>(span.data()), span.size());
}

/* Copy the values from the source buffer to all elements in the virtual array. */
inline void GVMutableArrayImpl::set_all(const void *src)
{
  this->set_all_impl(src);
}

template<typename T>
inline bool GVMutableArrayImpl::try_assign_VMutableArray(VMutableArray<T> &varray) const
{
  BLI_assert(type_->is<T>());
  return this->try_assign_VMutableArray_impl(&varray);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Inline methods for #GVArray.
 * \{ */

namespace detail {
template<typename StorageT> inline GVArrayAnyExtraInfo GVArrayAnyExtraInfo::get()
{
  static_assert(std::is_base_of_v<GVArrayImpl, StorageT> ||
                std::is_same_v<StorageT, const GVArrayImpl *> ||
                std::is_same_v<StorageT, std::shared_ptr<const GVArrayImpl>>);

  if constexpr (std::is_base_of_v<GVArrayImpl, StorageT>) {
    return {[](const void *buffer) {
      return static_cast<const GVArrayImpl *>((const StorageT *)buffer);
    }};
  }
  else if constexpr (std::is_same_v<StorageT, const GVArrayImpl *>) {
    return {[](const void *buffer) { return *(const StorageT *)buffer; }};
  }
  else if constexpr (std::is_same_v<StorageT, std::shared_ptr<const GVArrayImpl>>) {
    return {[](const void *buffer) { return ((const StorageT *)buffer)->get(); }};
  }
  else {
    BLI_assert_unreachable();
    return {};
  }
}
}  // namespace detail

inline GVArray::GVArray(const GVArray &other) : GVArrayCommon(other)
{
}

inline GVArray::GVArray(GVArray &&other) : GVArrayCommon(std::move(other))
{
}

inline GVArray::GVArray(const GVArrayImpl *impl) : GVArrayCommon(impl)
{
}

inline GVArray::GVArray(std::shared_ptr<const GVArrayImpl> impl) : GVArrayCommon(std::move(impl))
{
}

template<typename ImplT, typename... Args> inline GVArray GVArray::For(Args &&...args)
{
  static_assert(std::is_base_of_v<GVArrayImpl, ImplT>);
  GVArray varray;
  varray.template emplace<ImplT>(std::forward<Args>(args)...);
  return varray;
}

inline const GVArrayImpl *GVArray::operator->() const
{
  BLI_assert(*this);
  return impl_;
}

inline const GVArrayImpl &GVArray::operator*() const
{
  BLI_assert(*this);
  return *impl_;
}

template<typename T> inline GVArray::GVArray(const VArray<T> &varray)
{
  if (!varray) {
    return;
  }
  if (varray.try_assign_GVArray(*this)) {
    return;
  }
  if (varray.has_ownership()) {
    *this = GVArray::For<GVArrayImpl_For_VArray<T>>(varray);
  }
  else if (varray.is_span()) {
    Span<T> data = varray.get_internal_span();
    *this = GVArray::ForSpan(data);
  }
  else if (varray.is_single()) {
    T value = varray.get_internal_single();
    *this = GVArray::ForSingle(CPPType::get<T>(), varray.size(), &value);
  }
  else {
    *this = GVArray::For<GVArrayImpl_For_VArray<T>>(varray);
  }
}

template<typename T> inline VArray<T> GVArray::typed() const
{
  if (!*this) {
    return {};
  }
  BLI_assert(impl_->type().is<T>());
  VArray<T> varray;
  if (impl_->try_assign_VArray(varray)) {
    return varray;
  }
  if (impl_->has_ownership()) {
    return VArray<T>::template For<VArrayImpl_For_GVArray<T>>(*this);
  }
  if (impl_->is_span()) {
    const Span<T> span = impl_->get_internal_span().typed<T>();
    return VArray<T>::ForSpan(span);
  }
  if (impl_->is_single()) {
    T value;
    impl_->get_internal_single(&value);
    return VArray<T>::ForSingle(value, impl_->size());
  }
  return VArray<T>::template For<VArrayImpl_For_GVArray<T>>(*this);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Inline methods for #GVMutableArray.
 * \{ */

inline GVMutableArray::GVMutableArray(const GVMutableArray &other) : GVArrayCommon(other)
{
}

inline GVMutableArray::GVMutableArray(GVMutableArray &&other) : GVArrayCommon(std::move(other))
{
}

inline GVMutableArray::GVMutableArray(GVMutableArrayImpl *impl) : GVArrayCommon(impl)
{
}

inline GVMutableArray::GVMutableArray(std::shared_ptr<GVMutableArrayImpl> impl)
    : GVArrayCommon(std::move(impl))
{
}

template<typename ImplT, typename... Args>
inline GVMutableArray GVMutableArray::For(Args &&...args)
{
  static_assert(std::is_base_of_v<GVMutableArrayImpl, ImplT>);
  GVMutableArray varray;
  varray.emplace<ImplT>(std::forward<Args>(args)...);
  return varray;
}

inline GVMutableArrayImpl *GVMutableArray::operator->() const
{
  BLI_assert(*this);
  return this->get_impl();
}

inline GVMutableArrayImpl &GVMutableArray::operator*() const
{
  BLI_assert(*this);
  return *this->get_impl();
}

template<typename T> inline GVMutableArray::GVMutableArray(const VMutableArray<T> &varray)
{
  if (!varray) {
    return;
  }
  if (varray.try_assign_GVMutableArray(*this)) {
    return;
  }
  if (varray.has_ownership()) {
    *this = GVMutableArray::For<GVMutableArrayImpl_For_VMutableArray<T>>(varray);
  }
  else if (varray.is_span()) {
    MutableSpan<T> data = varray.get_internal_span();
    *this = GVMutableArray::ForSpan(data);
  }
  else {
    *this = GVMutableArray::For<GVMutableArrayImpl_For_VMutableArray<T>>(varray);
  }
}

template<typename T> inline VMutableArray<T> GVMutableArray::typed() const
{
  if (!*this) {
    return {};
  }
  GVMutableArrayImpl *impl = this->get_impl();
  BLI_assert(impl->type().is<T>());
  VMutableArray<T> varray;
  if (impl->try_assign_VMutableArray(varray)) {
    return varray;
  }
  if (impl->has_ownership()) {
    return VMutableArray<T>::template For<VMutableArrayImpl_For_GVMutableArray<T>>(*this);
  }
  if (impl->is_span()) {
    const MutableSpan<T> span = impl->get_internal_span().typed<T>();
    return VMutableArray<T>::ForSpan(span);
  }
  return VMutableArray<T>::template For<VMutableArrayImpl_For_GVMutableArray<T>>(*this);
}

/** \} */

}  // namespace blender::fn
