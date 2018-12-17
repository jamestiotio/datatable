//------------------------------------------------------------------------------
// Copyright 2018 H2O.ai
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
#include "expr/base_expr.h"
#include "expr/i_node.h"
#include "frame/py_frame.h"
#include "python/_all.h"
#include "python/string.h"
namespace dt {



//------------------------------------------------------------------------------
// allrows_in
//------------------------------------------------------------------------------

/**
 * i_node representing selection of all rows from a Frame.
 *
 * Although "all rows" selector can easily be implemented as a slice, we want
 * to have a separate class because (1) this is a very common selector type,
 * and (2) in some cases useful optimizations can be achieved if we know that
 * all rows were selected.
 */
class allrows_in : public i_node {
  public:
    allrows_in() = default;
    void execute(workframe&) override;
};


// All rows are selected, so no need to change the workframe.
void allrows_in::execute(workframe&) {}




//------------------------------------------------------------------------------
// onerow_in
//------------------------------------------------------------------------------

class onerow_in : public i_node {
  private:
    int64_t irow;

  public:
    onerow_in(int64_t i);
    void post_init_check(workframe&) override;
    void execute(workframe&) override;
};


onerow_in::onerow_in(int64_t i) : irow(i) {}


void onerow_in::post_init_check(workframe& wf) {
  int64_t inrows = static_cast<int64_t>(wf.nrows());
  if (irow < -inrows || irow >= inrows) {
    throw ValueError() << "Row `" << irow << "` is invalid for a frame with "
        << inrows << " row" << (inrows == 1? "" : "s");
  }
  if (irow < 0) irow += inrows;
}


void onerow_in::execute(workframe& wf) {
  size_t start = static_cast<size_t>(irow);
  wf.apply_rowindex(RowIndex(start, 1, 1));
}




//------------------------------------------------------------------------------
// slice_in
//------------------------------------------------------------------------------

class slice_in : public i_node {
  private:
    int64_t istart, istop, istep;
    bool is_slice;
    size_t : 56;

  public:
    slice_in(int64_t, int64_t, int64_t, bool);
    void execute(workframe&) override;
};


slice_in::slice_in(int64_t _start, int64_t _stop, int64_t _step, bool _slice)
{
  istart = _start;
  istop = _stop;
  istep = _step;
  is_slice = _slice;
}


void slice_in::execute(workframe& wf) {
  size_t nrows = wf.nrows();
  size_t start, count, step;
  if (is_slice) {
    py::oslice::normalize(nrows, istart, istop, istep, &start, &count, &step);
  }
  else {
    bool ok = py::orange::normalize(nrows, istart, istop, istep,
                                    &start, &count, &step);
    if (!ok) {
      throw ValueError() << "range(" << istart << ", " << istop << ", "
          << istep << ") cannot be applied to a Frame with " << nrows
          << " row" << (nrows == 1? "" : "s");
    }
  }
  wf.apply_rowindex(RowIndex(start, count, step));
}




//------------------------------------------------------------------------------
// expr_in
//------------------------------------------------------------------------------

class expr_in : public i_node {
  private:
    dt::base_expr* expr;

  public:
    explicit expr_in(py::robj src);
    ~expr_in() override;
    void execute(workframe&) override;
};


expr_in::expr_in(py::robj src) {
  expr = nullptr;
  py::oobj res = src.invoke("_core");
  xassert(res.typeobj() == &py::base_expr::Type::type);
  auto pybe = reinterpret_cast<py::base_expr*>(res.to_borrowed_ref());
  expr = pybe->release();
}


expr_in::~expr_in() {
  delete expr;
}


void expr_in::execute(workframe& wf) {
  SType st = expr->resolve(wf);
  if (st != SType::BOOL) {
    throw TypeError() << "Filter expression must be of `bool8` type, instead it "
        "was of type " << st;
  }
  Column* col = expr->evaluate_eager(wf);
  RowIndex res(col);
  wf.apply_rowindex(res);
  delete col;
}



//------------------------------------------------------------------------------
// frame_in
//------------------------------------------------------------------------------

class frame_in : public i_node {
  private:
    const DataTable* dt;  // borrowed ref

  public:
    explicit frame_in(py::robj src);
    void post_init_check(workframe&) override;
    void execute(workframe&) override;
};


frame_in::frame_in(py::robj src) {
  dt = src.to_frame();
  if (dt->ncols != 1) {
    throw ValueError() << "Only a single-column Frame may be used as `i` "
        "selector, instead got a Frame with " << dt->ncols << " columns";
  }
  SType st = dt->columns[0]->stype();
  if (!(st == SType::BOOL || info(st).ltype() == LType::INT)) {
    throw TypeError() << "A Frame which is used as an `i` selector should be "
        "either boolean or integer, instead got `" << st << "`";
  }
}


void frame_in::post_init_check(workframe& wf) {
  Column* col = dt->columns[0];
  size_t nrows = wf.nrows();
  if (col->stype() == SType::BOOL) {
    if (col->nrows != nrows) {
      throw ValueError() << "A boolean column used as `i` selector has "
          << col->nrows << " row" << (col->nrows == 1? "" : "s")
          << ", but applied to a Frame with "
           << nrows << " row" << (nrows == 1? "" : "s");
    }
  } else {
    int64_t min = col->min_int64();
    int64_t max = col->max_int64();
    if (min < -1) {
      throw ValueError() << "An integer column used as an `i` selector "
          "contains invalid negative indices: " << min;
    }
    if (max >= static_cast<int64_t>(nrows)) {
      throw ValueError() << "An integer column used as an `i` selector "
          "contains index " << max << " which is not valid for a Frame with "
          << nrows << " row" << (nrows == 1? "" : "s");
    }
  }
}


void frame_in::execute(workframe& wf) {
  RowIndex ri { dt->columns[0] };
  wf.apply_rowindex(ri);
}



//------------------------------------------------------------------------------
// nparray
//------------------------------------------------------------------------------

static i_node* _from_nparray(py::oobj src) {
  py::otuple shape = src.get_attr("shape").to_otuple();
  size_t ndims = shape.size();
  if (ndims == 2) {
    size_t dim0 = shape[0].to_size_t();
    size_t dim1 = shape[1].to_size_t();
    if (dim0 == 1 || dim1 == 1) {
      py::otuple args(1);
      args.set(0, py::oint(dim0 * dim1));
      src = src.invoke("reshape", args);
      shape = src.get_attr("shape").to_otuple();
      ndims = shape.size();
    }
  }
  if (ndims != 1) {
    throw ValueError() << "Only a single-dimensional numpy array is allowed "
        "as `i` selector, got array of shape " << shape;
  }
  py::ostring dtype = src.get_attr("dtype").to_pystring_force();
  std::string dtype_str { PyUnicode_AsUTF8(dtype.to_borrowed_ref()) };
  bool is_bool = dtype_str.compare(0, 4, "bool");
  bool is_int = dtype_str.compare(0, 3, "int");
  if (!(is_bool || is_int)) {
    throw TypeError() << "Either a boolean or an integer numpy array expected "
        "for an `i` selector, got array of dtype `" << dtype_str << "`";
  }
  // Now convert numpy array into a datatable Frame
  auto dt_Frame = py::oobj(reinterpret_cast<PyObject*>(&py::Frame::Type::type));
  py::otuple args(1);
  args.set(0, src);
  py::oobj frame = dt_Frame.call(args);
  return new frame_in(frame);
}




//------------------------------------------------------------------------------
// multislice_in
//------------------------------------------------------------------------------

class multislice_in : public i_node {
  private:
    enum class item_kind : size_t {
      INT, SLICE, RANGE
    };
    struct item {
      int64_t start, stop, step;
      item_kind kind;
    };
    std::vector<item> items;
    size_t min_nrows;

  public:
    explicit multislice_in(py::robj src);
    void post_init_check(workframe&) override;
    void execute(workframe&) override;
};


multislice_in::multislice_in(py::robj src) {
  size_t i = 0;
  size_t max_nrows = 0;
  for (auto elem : src.to_oiter()) {
    if (elem.is_int()) {
      int64_t value = elem.to_int64_strict();
      size_t n = (value >= 0)? static_cast<size_t>(value + 1)
                             : static_cast<size_t>(-value);
      if (n > max_nrows) max_nrows = n;
      items.push_back({value, 0, 0, item_kind::INT});
    }
    else if (elem.is_range()) {
      py::orange rr = elem.to_orange();
      int64_t start = rr.start();
      int64_t stop = rr.stop();
      int64_t step = rr.step();
      int64_t count = step > 0? (stop - start + step - 1) / step
                              : (start - stop - step - 1) / (-step);
      // Empty range, for example `range(5, 0)`. This is a valid object, but
      // it produces nothing. Hence, we'll just skip it.
      if (count <= 0) continue;
      // The first and the last element in the range must be either both
      // positive or both negative.
      int64_t last = start + (count - 1) * step;
      if ((start >= 0) != (last >= 0)) {
        throw ValueError() << "Invalid wrap-around range(" << start << ", "
            << stop << ", " << step << ") for an `i` selector";
      }
      stop = start + count * step;
      size_t n1 = static_cast<size_t>(start > 0? start + 1 : -start);
      size_t n2 = static_cast<size_t>(last > 0? last + 1 : -last);
      if (n1 > max_nrows) max_nrows = n1;
      if (n2 > max_nrows) max_nrows = n2;
      items.push_back({start, stop, step, item_kind::RANGE});
    }
    else if (elem.is_slice()) {
      py::oslice ss = elem.to_oslice();
      if (!ss.is_numeric()) {
        throw TypeError() << "Only integer-valued slices are allowed";
      }
      int64_t start = ss.start();
      int64_t stop = ss.stop();
      int64_t step = ss.step();
      if (step == 0) {
        if (stop < 0 || start == py::oslice::NA || stop == py::oslice::NA) {
          throw ValueError() << "Invalid " << ss << ": when step is 0, both "
              "start and stop must be present, and stop must be non-negative";
        }
      } else {
        if (step == py::oslice::NA) step = 1;
        if (start == py::oslice::NA) start = (step > 0)? 0 : py::oslice::MAX;
        if (stop == py::oslice::NA) stop = (step > 0)? py::oslice::MAX
                                                     : -py::oslice::MAX;
      }
      items.push_back({start, stop, step, item_kind::SLICE});
    }
    else {
      throw TypeError() << "Invalid item " << elem << " at index " << i
          << " in the `i` selector list";
    }
    i++;
  }
  min_nrows = max_nrows;
}


void multislice_in::post_init_check(workframe& wf) {
  if (wf.nrows() < min_nrows) {
    throw ValueError() << "`i` selector is valid for a Frame with at least "
        << min_nrows << " row" << (min_nrows == 1? "" : "s");
  }
}


void multislice_in::execute(workframe& wf) {
  int64_t inrows = static_cast<int64_t>(wf.nrows());
  size_t total_count = 0;
  for (auto& item : items) {
    switch (item.kind) {
      case item_kind::INT: {
        if (item.start < 0) item.start += inrows;
        xassert(item.start >= 0 && item.start < inrows);
        total_count++;
        break;
      }
      case item_kind::RANGE: {
        if (item.start < 0) item.start += inrows;
        if (item.stop < 0) item.stop += inrows;
        int64_t icount = (item.stop - item.start) / item.step;
        total_count += static_cast<size_t>(icount);
        break;
      }
      case item_kind::SLICE: {
        if (item.start < 0) item.start += inrows;
        if (item.start < 0) item.start = 0;
        if (item.start > inrows) continue;
        if (item.stop < 0) item.stop += inrows;
        if (item.stop < 0) item.stop = -1;
        if (item.stop > inrows) item.stop = inrows;
        int64_t icount = 0;
        if (item.step > 0 && item.stop > item.start) {
          icount = (item.stop - item.start + item.step - 1) / item.step;
        }
        if (item.step < 0 && item.stop < item.start) {
          icount = (item.start - item.stop - item.step - 1) / (-item.step);
        }
        total_count += static_cast<size_t>(icount);
        break;
      }
    }
  }
  arr32_t indices(total_count);
  int32_t* ind = indices.data();
  size_t j = 0;
  for (auto& item : items) {
    if (item.kind == item_kind::INT) {
      ind[j++] = static_cast<int32_t>(item.start);
    }
    else if (item.step > 0) {
      for (int64_t k = item.start; k < item.stop; k += item.step) {
        ind[j++] = static_cast<int32_t>(k);
      }
    }
    else if (item.step < 0) {
      for (int64_t k = item.start; k > item.stop; k += item.step) {
        ind[j++] = static_cast<int32_t>(k);
      }
    }
    else {
      for (int64_t k = 0; k < item.stop; k++) {
        ind[j++] = static_cast<int32_t>(item.start);
      }
    }
  }
  RowIndex ri(std::move(indices), false);
  wf.apply_rowindex(ri);
}




//------------------------------------------------------------------------------
// i_node
//------------------------------------------------------------------------------

i_node::~i_node() {}

void i_node::post_init_check(workframe&) {}


static i_node* _make(py::robj src) {
  // The most common case is `:`, a trivial slice
  if (src.is_slice()) {
    auto ssrc = src.to_oslice();
    if (ssrc.is_trivial()) return new allrows_in();
    if (ssrc.is_numeric()) {
      return new slice_in(ssrc.start(), ssrc.stop(), ssrc.step(), true);
    }
    throw TypeError() << src << " is not integer-valued";
  }
  if (is_PyBaseExpr(src)) {
    return new expr_in(src);
  }
  if (src.is_frame()) {
    return new frame_in(src);
  }
  if (src.is_int()) {
    int64_t val = src.to_int64_strict();
    return new onerow_in(val);
  }
  if (src.is_none() || src.is_ellipsis()) {
    return new allrows_in();
  }
  if (src.is_numpy_array()) {
    return _from_nparray(src);
  }
  if (src.is_range()) {
    auto ss = src.to_orange();
    return new slice_in(ss.start(), ss.stop(), ss.step(), false);
  }
  // "iterable" is a very generic interface, so it must come close to last
  // in the resolution sequence
  if (src.is_iterable()) {
    return new multislice_in(src);
  }
  if (src.is_bool()) {
    throw TypeError() << "Boolean value cannot be used as an `i` expression";
  }
  return nullptr;  // for now
}


iptr i_node::make(py::robj src) {
  return iptr(_make(src));
}


}  // namespace dt
