//------------------------------------------------------------------------------
// Copyright 2018-2022 H2O.ai
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//------------------------------------------------------------------------------
#ifndef dt_COLUMN_COLUMN_IMPL_h
#define dt_COLUMN_COLUMN_IMPL_h
#include <memory>        // std::unique_ptr
#include "python/obj.h"  // py::robj
#include "column.h"      // Column, NaStorage, colvec
#include "groupby.h"     // Groupby
#include "buffer.h"      // Buffer
#include "stats.h"       // Stats
#include "types/type.h"
namespace dt {



/**
  * "Column Implementation": this class realizes the Pimpl idiom for
  * the `Column` class, and serves as the base in the hierarchy of
  * different column implementation classes.
  *
  * The derived classes are required to implement the following
  * required methods:
  *
  *   - `clone()` : creates a copy of the object;
  *   - `get_element()` : the class may implement one or more of these
  *       methods, depending on which stypes it supports;
  *   - `memory_footprint()` : compute the size of class and its data;
  *
  * In addition, the derived class may reimplement any other virtual
  * methods, depending on its capabilities.
  *
  */
class ColumnImpl
{
  protected:
    Type   type_;
    size_t nrows_;
    mutable size_t refcount_;
    mutable std::unique_ptr<Stats> stats_;

  //------------------------------------
  // Constructors
  //------------------------------------
  public:
    ColumnImpl(size_t nrows, Type type);
    ColumnImpl(size_t nrows, SType stype);
    ColumnImpl(const ColumnImpl&) = delete;
    ColumnImpl(ColumnImpl&&) = delete;
    virtual ~ColumnImpl() = default;

    virtual ColumnImpl* clone() const = 0;
    virtual void materialize(Column& out, bool to_memory);
    virtual void verify_integrity() const;
    virtual bool allow_parallel_access() const;


  //------------------------------------
  // Element access
  //------------------------------------
  public:
    virtual bool get_element(size_t i, int8_t* out) const;
    virtual bool get_element(size_t i, int16_t* out) const;
    virtual bool get_element(size_t i, int32_t* out) const;
    virtual bool get_element(size_t i, int64_t* out) const;
    virtual bool get_element(size_t i, float* out) const;
    virtual bool get_element(size_t i, double* out) const;
    virtual bool get_element(size_t i, CString* out) const;
    virtual bool get_element(size_t i, py::oobj* out) const;
    virtual bool get_element(size_t i, Column* out) const;


  //------------------------------------
  // Properties
  //------------------------------------
  public:
    size_t nrows() const noexcept { return nrows_; }
    SType stype() const { return type_.stype(); }
    SType data_stype() const;
    const Type& type() const { return type_; }
    virtual bool is_virtual() const noexcept = 0;
    virtual bool computationally_expensive() const { return false; }
    virtual size_t memory_footprint() const noexcept = 0;

    virtual size_t n_children() const noexcept = 0;
    virtual const Column& child(size_t i) const;
    Stats* stats() const;
    virtual size_t null_count() const;

  //------------------------------------
  // Data buffers
  //------------------------------------
  public:
    virtual NaStorage   get_na_storage_method() const noexcept = 0;
    virtual size_t      get_num_data_buffers() const noexcept = 0;
    virtual bool        is_data_editable(size_t k) const = 0;
    virtual size_t      get_data_size(size_t k) const = 0;
    virtual const void* get_data_readonly(size_t k) const = 0;
    virtual void*       get_data_editable(size_t k) = 0;
    virtual Buffer      get_data_buffer(size_t k) const = 0;

    virtual void save_to_jay(ColumnJayData& cj) = 0;


  //------------------------------------
  // Column manipulation
  //------------------------------------
  public:
    virtual void fill_npmask(bool* outmask, size_t row0, size_t row1) const;
    virtual void sort_grouped(const Groupby&, Column& out);

    virtual void repeat(size_t ntimes, Column& out);
    virtual void na_pad(size_t new_nrows, Column& out);
    virtual void truncate(size_t new_nrows, Column& out);
    virtual void apply_rowindex(const RowIndex& ri, Column& out);
    virtual void replace_values(const RowIndex& replace_at,
                                const Column& replace_with, Column& out);
    virtual void pre_materialize_hook() {}

    // cast_replace(new_type, thiscol)
    //   Cast the column into new type, storing the result into
    //   `thiscol`. An implementation may move `thiscol` before
    //   storing a new Column there.
    //
    //   An implementation is allowed to cast into slightly different
    //   stype (but within the ltype), for example when casting to
    //   str32 but the result may only fit into str64.
    //
    //   This method is const, and should not modify the column's
    //   data. If it is desired to change the column's type
    //   "in place", the method should make a clone first.
    //
    virtual void cast_replace(Type new_type, Column& thiscol) const;

    virtual Column as_arrow() const;


  //------------------------------------
  // Private helpers
  //------------------------------------
  private:
    // TODO: this should really be materializer for RBound column impl
    virtual void rbind_impl(colvec& columns, size_t nrows, bool isempty,
                            SType& cast_stype);

    template <typename T> void _materialize_fw(Column&);
    void _materialize_str(Column&);
    void _materialize_obj(Column&);

    template <typename T>
    void _fill_npmask(bool* outmask, size_t row0, size_t row1) const;

    Column _as_arrow_void() const;
    Column _as_arrow_bool() const;
    template <typename T> Column _as_arrow_fw() const;
    template <typename T> Column _as_arrow_str() const;

    friend class ::Column;
};




} // namespace dt
#endif
