#!/usr/bin/env python3
# Copyright 2017 H2O.ai; Apache License Version 2.0;  -*- encoding: utf-8 -*-

# noinspection PyUnresolvedReferences
import _datatable as c
import datatable
from .mrblock import MRBlock
from .llvm import inject_c_code



class EvaluationModule(object):
    """
    Manager class for rendering expression nodes into C code.
    """
    _counter = 0

    def __init__(self, rows=None):
        self._stack = []
        self._outputs = []
        self._functions = []
        self._filter_block = None
        self._output_block = MRBlock(self)
        self._rows_expr = None
        self._var_counter = 0
        self.rowmapping = None
        self.set_rows_expression(rows)


    def set_rows_expression(self, expr):
        if expr is None:
            return
        if self._rows_expr is not None:
            raise ValueError("Filter is already set")
        n = len(self._stack)
        self._rows_expr = expr
        self._filter_block = MRBlock(self)
        self._functions.append(self._filter_block)
        self._stack.append("rowmapping_nrows")
        self._stack.append("rowmapping")
        testvar = expr.value_or_0(self._filter_block)
        self._filter_block.set_filter(testvar)
        alloc_size = self._filter_block.add_filter_output(n)
        if alloc_size:
            self._stack[n + 1] = ("rowmapping", alloc_size)


    def add_output_column(self, expr):
        r = expr.value(self._output_block)
        self._outputs.append(r)


    def add_mrblock(self, fn):
        self._functions.insert(0, fn)

    def use_datatable(self, dt):
        """
        Register datatable and make it available during evaluation.

        The code that will be generated in the module have all functions take
        ``DataTable*`` array as the first argument. The array will be filled
        in during runtime (in C). This function returns the index at which
        datatable ``dt`` will appear in this array.
        """
        try:
            return self._stack.index(dt)
        except ValueError:
            self._stack.append(dt)
            return len(self._stack) - 1


    def add_stack_variable(self, name):
        n = len(self._stack)
        self._stack.append(name)
        return n

    def next_fun_counter(self):
        EvaluationModule._counter += 1
        return EvaluationModule._counter

    def next_var_counter(self):
        self._var_counter += 1
        return self._var_counter


    def generate_c_code(self):
        out = _module_header
        out += "\n"
        out += "/**\n"
        out += " * rows:\n"
        out += " *   %s\n" % self._rows_expr
        out += " *\n"
        out += " * stack:\n"
        for i, st in enumerate(self._stack):
            if isinstance(st, tuple):
                st = "%s  (alloc: %d)" % st
            out += " *   [%d] = %s\n" % (i, st)
        out += " **/\n"
        out += "\n\n"
        for fn in self._functions:
            out += fn.generate_c_code()
            out += "\n\n"
        return out


    def run(self, verbose=False):
        cc = self.generate_c_code()
        if verbose:
            print("C code generated:")
            print("-" * 80)
            print(cc)
            print("-" * 80)
        funcs = [fn.function_name for fn in self._functions]
        ptrs = inject_c_code(cc, funcs)
        assert len(ptrs) == len(funcs)
        stack = [
            x.internal if isinstance(x, datatable.DataTable) else
            x if isinstance(x, c.RowMapping) else
            x[1] if isinstance(x, tuple) else
            None
            for x in self._stack
        ]
        ceval = c.Evaluator()
        ceval.generate_stack(stack)
        for fnptr in ptrs:
            ceval.run_mbr(fnptr, self._filter_block._nrows)
        idx_nrows = self._stack.index("rowmapping_nrows")
        res_nrows = ceval.get_stack_value(idx_nrows, 1)
        self.rowmapping = ceval.get_stack_value(idx_nrows, 257)



_module_header = """/**
 *  This code is generated by datatable/exec/module.py
 **/
#include <stdlib.h>  // intNN_t, etc.

typedef int RowMappingType;
typedef int DataSType;

typedef struct RowMapping {
    RowMappingType type;
    int64_t length;
    union {
        int64_t *indices;
        struct { int64_t start, step; } slice;
    };
} RowMapping;

typedef struct Column {
    void *data;
    DataSType type;
    void *meta;
    int64_t srcindex;
    int8_t mmapped;
} Column;

typedef struct DataTable {
  int64_t nrows, ncols;
  struct DataTable *source;
  struct RowMapping *rowmapping;
  struct Column *columns;
} DataTable;

typedef union Value {
  int64_t i8;
  int32_t i4;
  int16_t i2;
  int8_t  i1;
  double  f8;
  float   f4;
  Column *col;
  DataTable *dt;
  RowMapping *rowmap;
  void   *ptr;
} Value;

typedef union { uint64_t i; double d; } double_repr;
typedef union { uint32_t i; float f; } float_repr;
static inline int ISNA_F32(float x) { float_repr xx; xx.f = x; return xx.i == 0x7F8007A2u; }
static inline int ISNA_F64(double x) { double_repr xx; xx.d = x; return xx.i == 0x7FF00000000007A2ull; }
static inline float __nanf__(void) { const float_repr x = { 0x7F8007A2ul }; return x.f; }
static inline double __nand__(void) { const double_repr x = { 0x7FF00000000007A2ull }; return x.d; }

#define NA_I8    (-128)
#define NA_I16   (-32768)
#define NA_I32   (-2147483647-1)
#define NA_I64   (-9223372036854775807-1)
#define NA_UI8   255u
#define NA_UI16  65535u
#define NA_UI32  4294967295u
#define NA_UI64  18446744073709551615u
#define NA_F32   __nanf__()
#define NA_F64   __nand__()

"""

