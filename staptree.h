// -*- C++ -*-
// Copyright (C) 2005-2017 Red Hat Inc.
// Copyright (C) 2006 Intel Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#ifndef STAPTREE_H
#define STAPTREE_H

#include <map>
#include <memory>
#include <stack>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cassert>
#include <typeinfo>
extern "C" {
#include <stdint.h>
}

#include "util.h"
#include "stringtable.h"


struct token; // parse.h
struct systemtap_session; // session.h

struct semantic_error: public std::runtime_error
{
  const token* tok1;
  const token* tok2;
  std::string errsrc;

  // Extra details to explain the error or provide alternatives to the user.
  // Each one printed after the main error message and tokens aligned on
  // separate lines. Just push_back anything you want that better explains
  // the error to the user (not meant for extra verbose developer messages).
  std::vector<std::string> details;

  ~semantic_error () throw ()
    {
      if (chain)
        delete chain;
    }

  semantic_error (const std::string& src, const std::string& msg, const token* t1=0):
    runtime_error (msg), tok1 (t1), tok2 (0), errsrc (src), chain (0) {}

  semantic_error (const std::string& src, const std::string& msg, const token* t1,
                  const token* t2, const semantic_error* chn=0):
      runtime_error (msg), tok1 (t1), tok2 (t2), errsrc (src), chain (0)
    {
      if (chn)
        set_chain(*chn);
    }

  /* override copy-ctor to deep-copy chain */
  semantic_error (const semantic_error& other):
      runtime_error(other), tok1(other.tok1), tok2(other.tok2),
      errsrc(other.errsrc), details(other.details), chain (0)
    {
      if (other.chain)
        set_chain(*other.chain);
    }

  std::string errsrc_chain(void) const
    {
      return errsrc + (chain ? "|" + chain->errsrc_chain() : "");
    }

  semantic_error& set_chain(const semantic_error& new_chain)
    {
      if (chain)
        delete chain;
      chain = new semantic_error(new_chain);
      return *this;
    }

  const semantic_error* get_chain(void) const
    {
      return chain;
    }

private:
  const semantic_error* chain;
};

// ------------------------------------------------------------------------

/* struct statistic_decl moved to session.h */

// ------------------------------------------------------------------------

enum exp_type
  {
    pe_unknown,
    pe_long,   // int64_t
    pe_string, // std::string
    pe_stats
  };

std::ostream& operator << (std::ostream& o, const exp_type& e);

struct functioncall;
struct autocast_op;
struct exp_type_details
{
  virtual ~exp_type_details () {};

  // A process-wide unique identifier; probably a pointer.
  virtual uintptr_t id () const = 0;
  bool operator==(const exp_type_details& other) const
    { return id () == other.id (); }
  bool operator!=(const exp_type_details& other) const
    { return !(*this == other); }

  // Expand this autocast_op into a function call
  virtual bool expandable() const = 0;
  virtual functioncall *expand(autocast_op* e, bool lvalue) = 0;
};
typedef std::shared_ptr<exp_type_details> exp_type_ptr;


struct token;
struct visitor;
struct update_visitor;

struct visitable
{
  virtual ~visitable ();
};

struct symbol;
struct expression : public visitable
{
  exp_type type;
  exp_type_ptr type_details;
  const token* tok;
  expression ();
  virtual ~expression ();
  // NB: this pretty-printer function must generate such text that
  // allows distinguishing operationally-different instances.  During
  // stap -p2/-p3, function bodies with identical pretty-printed
  // bodies may be duplicate-eliminated.  So print any relevant member
  // variables somehow.
  virtual void print (std::ostream& o) const = 0;
  virtual void visit (visitor* u) = 0;
  virtual bool is_symbol(symbol *& sym_out);
};

std::ostream& operator << (std::ostream& o, const expression& k);


struct literal: public expression
{
};


struct literal_string: public literal
{
  interned_string value;
  literal_string (interned_string v);
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct literal_number: public literal
{
  int64_t value;
  bool print_hex;
  literal_number (int64_t v, bool hex=false);
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct embedded_expr: public expression
{
  interned_string code;
  bool tagged_p (const char *tag) const;
  bool tagged_p (const interned_string& tag) const;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct binary_expression: public expression
{
  expression* left;
  interned_string op;
  expression* right;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct unary_expression: public expression
{
  interned_string op;
  expression* operand;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct pre_crement: public unary_expression
{
  void visit (visitor* u);
};


struct post_crement: public unary_expression
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct logical_or_expr: public binary_expression
{
  void visit (visitor* u);
};


struct logical_and_expr: public binary_expression
{
  void visit (visitor* u);
};


struct arrayindex;
struct array_in: public expression
{
  arrayindex* operand;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct regex_query: public expression
{
  expression* left;
  interned_string op;
  literal_string* right;
  void visit (visitor* u);
  void print (std::ostream& o) const;
};

struct compound_expression: public binary_expression
{
  compound_expression() { op = ","; }
  void visit (visitor* u);
};

struct comparison: public binary_expression
{
  void visit (visitor* u);
};

struct concatenation: public binary_expression
{
  void visit (visitor* u);
};


struct ternary_expression: public expression
{
  expression* cond;
  expression* truevalue;
  expression* falsevalue;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct assignment: public binary_expression
{
  void visit (visitor* u);
};

struct hist_op;
struct indexable : public expression
{
  // This is a helper class which, type-wise, acts as a disjoint union
  // of symbols and histograms. You can ask it whether it's a
  // histogram or a symbol, and downcast accordingly.
  virtual bool is_symbol(symbol *& sym_out);
  virtual bool is_hist_op(hist_op *& hist_out);
  virtual ~indexable() {}
};

// Perform a downcast to one out-value and NULL the other, throwing an
// exception if neither downcast succeeds. This is (sadly) about the
// best we can accomplish in C++.
void
classify_indexable(indexable* ix,
		   symbol *& array_out,
		   hist_op *& hist_out);

struct vardecl;
struct symbol: public indexable
{
  interned_string name;
  vardecl *referent;
  symbol ();
  void print (std::ostream& o) const;
  void visit (visitor* u);
  // overrides of type 'indexable'
  bool is_symbol(symbol *& sym_out);
};

struct target_register: public expression
{
  unsigned regno;
  bool userspace_p;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct target_deref: public expression
{
  expression* addr;
  unsigned size;
  bool signed_p;
  bool userspace_p;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct target_bitfield: public expression
{
  expression* base;
  unsigned offset;
  unsigned size;
  bool signed_p;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct target_symbol: public expression
{
  enum component_type
    {
      comp_struct_member,
      comp_literal_array_index,
      comp_expression_array_index,
      comp_pretty_print, // must be final
    };

  struct component
    {
      const token* tok;
      component_type type;
      std::string member; // comp_struct_member, comp_pretty_print
      int64_t num_index; // comp_literal_array_index
      expression* expr_index; // comp_expression_array_index

      component(const token* t, const std::string& m, bool pprint=false):
        tok(t),
        type(pprint ? comp_pretty_print : comp_struct_member),
        member(m), num_index(0), expr_index(0)
      {}
      component(const token* t, int64_t n):
        tok(t), type(comp_literal_array_index), num_index(n),
        expr_index(0) {}
      component(const token* t, expression* e):
        tok(t), type(comp_expression_array_index), num_index(0),
         expr_index(e) {}
      void print (std::ostream& o) const;
    };

  interned_string name;
  bool addressof;
  bool synthetic;
  std::vector<component> components;
  semantic_error* saved_conversion_error; // hand-made linked list
  target_symbol(): addressof(false), synthetic(false), saved_conversion_error (0) {}
  virtual std::string sym_name ();
  void chain (const semantic_error& er);
  void print (std::ostream& o) const;
  void visit (visitor* u);
  void visit_components (visitor* u);
  void visit_components (update_visitor* u);
  void assert_no_components(const std::string& tapset, bool pretty_ok=false);
  size_t pretty_print_depth () const;
  bool check_pretty_print (bool lvalue=false) const;
};

std::ostream& operator << (std::ostream& o, const target_symbol::component& c);


struct cast_op: public target_symbol
{
  expression *operand;
  interned_string type_name, module;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

// An autocast is like an implicit @cast on any expression, like
// (expr)->foo->var[baz], and the type is gleaned from the expr.
struct autocast_op: public target_symbol
{
  expression *operand;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct atvar_op: public target_symbol
{
  interned_string target_name, cu_name, module;
  virtual std::string sym_name ();
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct defined_op: public expression
{
  expression *operand;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

struct entry_op: public expression
{
  expression *operand;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct perf_op: public expression
{
  literal_string *operand;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct arrayindex: public expression
{
  std::vector<expression*> indexes;
  indexable *base;
  arrayindex ();
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct functiondecl;
struct functioncall: public expression
{
  interned_string function;
  std::vector<expression*> args;
  std::vector<functiondecl*> referents;
  functioncall ();
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct print_format: public expression
{
  bool print_to_stream;
  bool print_with_format;
  bool print_with_delim;
  bool print_with_newline;
  bool print_char;

  // XXX match runtime/vsprintf.c's print_flag
  // ... for use with number() & number_size()
  enum format_flag
    {
      fmt_flag_zeropad = 1,
      fmt_flag_sign = 2,
      fmt_flag_plus = 4,
      fmt_flag_space = 8,
      fmt_flag_left = 16,
      fmt_flag_special = 32,
      fmt_flag_large = 64,
    };

  enum conversion_type
    {
      conv_unspecified,
      conv_pointer,
      conv_number,
      conv_string,
      conv_char,
      conv_memory,
      conv_memory_hex,
      conv_literal,
      conv_binary
    };

  enum width_type
    {
      width_unspecified,
      width_static,
      width_dynamic
    };

  enum precision_type
    {
      prec_unspecified,
      prec_static,
      prec_dynamic
    };

  struct format_component
  {
    unsigned base;
    unsigned width;
    unsigned precision;
    unsigned flags : 8;
    width_type widthtype : 8;
    precision_type prectype : 8;
    conversion_type type : 8;
    interned_string literal_string;
    bool is_empty() const
    {
      return flags == 0
	&& widthtype == width_unspecified
	&& prectype == prec_unspecified
	&& type == conv_unspecified
	&& literal_string.empty();
    }
    void clear()
    {
      base = 0;
      flags = 0;
      width = 0;
      widthtype = width_unspecified;
      precision = 0;
      prectype = prec_unspecified;
      type = conv_unspecified;
      literal_string.clear();
    }
    format_component() { clear(); }
    inline void set_flag(format_flag f) { flags |= f; }
    inline bool test_flag(format_flag f) const { return flags & f; }
  };

  std::string raw_components;
  std::vector<format_component> components;
  interned_string delimiter;
  std::vector<expression*> args;
  hist_op *hist;

  static std::string components_to_string(std::vector<format_component> const & components);
  static std::vector<format_component> string_to_components(std::string const & str);
  static print_format* create(const token *t, const char *n = NULL);

  void print (std::ostream& o) const;
  void visit (visitor* u);

private:
  interned_string print_format_type;
  print_format(bool stream, bool format, bool delim, bool newline, bool _char, interned_string type):
    print_to_stream(stream), print_with_format(format),
    print_with_delim(delim), print_with_newline(newline),
    print_char(_char), hist(NULL), print_format_type(type)
  {}
};


enum stat_component_type
  {
    sc_average,
    sc_count,
    sc_sum,
    sc_min,
    sc_max,
    sc_none,
    sc_variance,
  };

struct stat_op: public expression
{
  stat_component_type ctype;
  expression* stat;
  std::vector<int64_t> params;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};

enum histogram_type
  {
    hist_linear,
    hist_log
  };

struct hist_op: public indexable
{
  histogram_type htype;
  expression* stat;
  std::vector<int64_t> params;
  void print (std::ostream& o) const;
  void visit (visitor* u);
  // overrides of type 'indexable'
  bool is_hist_op(hist_op *& hist_out);
};

// ------------------------------------------------------------------------


struct symboldecl // unique object per (possibly implicit)
		  // symbol declaration
{
  const token* tok;
  const token* systemtap_v_conditional; //checking systemtap compatibility
  interned_string name; // mangled name
  interned_string unmangled_name;
  exp_type type;
  exp_type_ptr type_details;
  symboldecl ();
  virtual ~symboldecl ();
  virtual void print (std::ostream &o) const = 0;
  virtual void printsig (std::ostream &o) const = 0;
};


std::ostream& operator << (std::ostream& o, const symboldecl& k);


struct vardecl: public symboldecl
{
  void print (std::ostream& o) const;
  void printsig (std::ostream& o) const;
  vardecl ();
  void set_arity (int arity, const token* t);
  bool compatible_arity (int a);
  const token* arity_tok; // site where arity was first resolved
  int arity; // -1: unknown; 0: scalar; >0: array
  int maxsize; // upperbound on size for arrays
  std::vector<exp_type> index_types; // for arrays only
  literal *init; // for global scalars only
  bool synthetic; // for probe locals only, don't init on entry
  bool wrap;
  bool char_ptr_arg; // set in ::emit_common_header(), only used if a formal_arg
};


struct vardecl_builtin: public vardecl
{
};

struct statement;
struct functiondecl: public symboldecl
{
  std::vector<vardecl*> formal_args;
  std::vector<vardecl*> locals;
  std::vector<vardecl*> unused_locals;
  statement* body;
  bool synthetic;
  bool mangle_oldstyle;
  bool has_next;
  int64_t priority;
  functiondecl ();
  void print (std::ostream& o) const;
  void printsig (std::ostream& o) const;
  void printsigtags (std::ostream& o, bool all_tags) const;
  void join (systemtap_session& s); // for synthetic functions only
};

struct function_priority_order
{
  bool operator() (const functiondecl* f1, const functiondecl* f2)
  {
    return f1->priority < f2->priority;
  }
};


// ------------------------------------------------------------------------


struct statement : public visitable
{
  virtual void print (std::ostream& o) const = 0;
  virtual void visit (visitor* u) = 0;
  const token* tok;
  statement ();
  statement (const token* tok);
  virtual ~statement ();
};

std::ostream& operator << (std::ostream& o, const statement& k);


struct embeddedcode: public statement
{
  interned_string code;
  bool tagged_p (const char *tag) const;
  bool tagged_p (const interned_string& tag) const;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct block: public statement
{
  std::vector<statement*> statements;
  void print (std::ostream& o) const;
  void visit (visitor* u);
  block () {}
  block (statement* car, statement* cdr);
  virtual ~block () {}
};


struct try_block: public statement
{
  statement* try_block; // may be 0
  statement* catch_block; // may be 0
  symbol* catch_error_var; // may be 0
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct expr_statement;
struct for_loop: public statement
{
  expr_statement* init; // may be 0
  expression* cond;
  expr_statement* incr; // may be 0
  statement* block;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct foreach_loop: public statement
{
  // this part is a specialization of arrayindex
  std::vector<symbol*> indexes;
  std::vector<expression*> array_slice; // optional array slice to iterate over
  indexable *base;
  int sort_direction; // -1: decreasing, 0: none, 1: increasing
  unsigned sort_column; // 0: value, 1..N: index
  enum stat_component_type sort_aggr; // for aggregate arrays, which aggregate to sort on
  symbol* value; // optional iteration value
  expression* limit; // optional iteration limit

  statement* block;
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct null_statement: public statement
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
  null_statement (const token* tok);
};


struct expr_statement: public statement
{
  expression* value;  // executed for side-effects
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct if_statement: public statement
{
  expression* condition;
  statement* thenblock;
  statement* elseblock; // may be 0
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct return_statement: public expr_statement
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct delete_statement: public expr_statement
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct break_statement: public statement
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct continue_statement: public statement
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct next_statement: public statement
{
  void print (std::ostream& o) const;
  void visit (visitor* u);
};


struct probe;
struct derived_probe;
struct probe_alias;
struct embeddedcode;
struct stapfile
{
  std::string name;
  std::vector<probe*> probes;
  std::vector<probe_alias*> aliases;
  std::vector<functiondecl*> functions;
  std::vector<vardecl*> globals;
  std::vector<embeddedcode*> embeds;
  interned_string file_contents;
  bool privileged;
  bool synthetic; // via parse_synthetic_*
  stapfile ():
    privileged (false), synthetic (false) {}
  void print (std::ostream& o) const;
};


struct probe_point
{
  struct component // XXX: sort of a restricted functioncall
  {
    interned_string functor;
    literal* arg; // optional
    bool from_glob;
    component ();
    const token* tok; // points to component's functor
    component(interned_string f, literal *a=NULL, bool from_glob=false);
  };
  std::vector<component*> components;
  bool optional;
  bool sufficient;
  bool well_formed; // used in derived_probe::script_location()
  expression* condition;
  std::string auto_path;
  void print (std::ostream& o, bool print_extras=true) const;
  probe_point ();
  probe_point(const probe_point& pp);
  probe_point(std::vector<component*> const & comps);
  std::string str(bool print_extras=true) const;
  bool from_globby_comp(const std::string& comp);
};

std::ostream& operator << (std::ostream& o, const probe_point& k);


struct probe
{
  static unsigned last_probeidx;

  std::vector<probe_point*> locations;
  statement* body;
  struct probe* base;
  const token* tok;
  const token* systemtap_v_conditional; //checking systemtap compatibility
  std::vector<vardecl*> locals;
  std::vector<vardecl*> unused_locals;
  bool privileged;
  bool synthetic;
  unsigned id;

  probe ();
  probe (probe* p, probe_point *l);
  void print (std::ostream& o) const;
  std::string name () const;
  virtual void printsig (std::ostream &o) const;
  virtual void collect_derivation_chain (std::vector<probe*> &probes_list) const;
  virtual void collect_derivation_pp_chain (std::vector<probe_point*> &) const;
  virtual const probe_alias *get_alias () const { return 0; }
  virtual probe_point *get_alias_loc () const { return 0; }
  virtual ~probe() {}

private:

  probe (const probe&);
  probe& operator = (const probe&);
};

struct probe_alias: public probe
{
  probe_alias(std::vector<probe_point*> const & aliases);
  std::vector<probe_point*> alias_names;
  virtual void printsig (std::ostream &o) const;
  bool epilogue_style;
};


// A derived visitor instance is used to visit the entire
// statement/expression tree.
struct visitor
{
  // Machinery for differentiating lvalue visits from non-lvalue.
  std::vector<expression *> active_lvalues;
  bool is_active_lvalue(expression *e);
  void push_active_lvalue(expression *e);
  void pop_active_lvalue();

  virtual ~visitor () {}
  virtual void visit_block (block *s) = 0;
  virtual void visit_try_block (try_block *s) = 0;
  virtual void visit_embeddedcode (embeddedcode *s) = 0;
  virtual void visit_null_statement (null_statement *s) = 0;
  virtual void visit_expr_statement (expr_statement *s) = 0;
  virtual void visit_if_statement (if_statement* s) = 0;
  virtual void visit_for_loop (for_loop* s) = 0;
  virtual void visit_foreach_loop (foreach_loop* s) = 0;
  virtual void visit_return_statement (return_statement* s) = 0;
  virtual void visit_delete_statement (delete_statement* s) = 0;
  virtual void visit_next_statement (next_statement* s) = 0;
  virtual void visit_break_statement (break_statement* s) = 0;
  virtual void visit_continue_statement (continue_statement* s) = 0;
  virtual void visit_literal_string (literal_string* e) = 0;
  virtual void visit_literal_number (literal_number* e) = 0;
  virtual void visit_embedded_expr (embedded_expr* e) = 0;
  virtual void visit_binary_expression (binary_expression* e) = 0;
  virtual void visit_unary_expression (unary_expression* e) = 0;
  virtual void visit_pre_crement (pre_crement* e) = 0;
  virtual void visit_post_crement (post_crement* e) = 0;
  virtual void visit_logical_or_expr (logical_or_expr* e) = 0;
  virtual void visit_logical_and_expr (logical_and_expr* e) = 0;
  virtual void visit_array_in (array_in* e) = 0;
  virtual void visit_regex_query (regex_query* e) = 0;
  virtual void visit_compound_expression (compound_expression * e) = 0;
  virtual void visit_comparison (comparison* e) = 0;
  virtual void visit_concatenation (concatenation* e) = 0;
  virtual void visit_ternary_expression (ternary_expression* e) = 0;
  virtual void visit_assignment (assignment* e) = 0;
  virtual void visit_symbol (symbol* e) = 0;
  virtual void visit_target_register (target_register* e) = 0;
  virtual void visit_target_deref (target_deref* e) = 0;
  virtual void visit_target_bitfield (target_bitfield* e) = 0;
  virtual void visit_target_symbol (target_symbol* e) = 0;
  virtual void visit_arrayindex (arrayindex* e) = 0;
  virtual void visit_functioncall (functioncall* e) = 0;
  virtual void visit_print_format (print_format* e) = 0;
  virtual void visit_stat_op (stat_op* e) = 0;
  virtual void visit_hist_op (hist_op* e) = 0;
  virtual void visit_cast_op (cast_op* e) = 0;
  virtual void visit_autocast_op (autocast_op* e) = 0;
  virtual void visit_atvar_op (atvar_op* e) = 0;
  virtual void visit_defined_op (defined_op* e) = 0;
  virtual void visit_entry_op (entry_op* e) = 0;
  virtual void visit_perf_op (perf_op* e) = 0;
};


// A NOP visitor doesn't do anything.  It's a useful base class if you need to
// take action only for specific types, without even traversing the rest.
struct nop_visitor: public visitor
{
  virtual ~nop_visitor () {}
  virtual void visit_block (block *) {};
  virtual void visit_try_block (try_block *) {};
  virtual void visit_embeddedcode (embeddedcode *) {};
  virtual void visit_null_statement (null_statement *) {};
  virtual void visit_expr_statement (expr_statement *) {};
  virtual void visit_if_statement (if_statement*) {};
  virtual void visit_for_loop (for_loop*) {};
  virtual void visit_foreach_loop (foreach_loop*) {};
  virtual void visit_return_statement (return_statement*) {};
  virtual void visit_delete_statement (delete_statement*) {};
  virtual void visit_next_statement (next_statement*) {};
  virtual void visit_break_statement (break_statement*) {};
  virtual void visit_continue_statement (continue_statement*) {};
  virtual void visit_literal_string (literal_string*) {};
  virtual void visit_literal_number (literal_number*) {};
  virtual void visit_embedded_expr (embedded_expr*) {};
  virtual void visit_binary_expression (binary_expression*) {};
  virtual void visit_unary_expression (unary_expression*) {};
  virtual void visit_pre_crement (pre_crement*) {};
  virtual void visit_post_crement (post_crement*) {};
  virtual void visit_logical_or_expr (logical_or_expr*) {};
  virtual void visit_logical_and_expr (logical_and_expr*) {};
  virtual void visit_array_in (array_in*) {};
  virtual void visit_regex_query (regex_query*) {};
  virtual void visit_compound_expression (compound_expression *) {};
  virtual void visit_comparison (comparison*) {};
  virtual void visit_concatenation (concatenation*) {};
  virtual void visit_ternary_expression (ternary_expression*) {};
  virtual void visit_assignment (assignment*) {};
  virtual void visit_symbol (symbol*) {};
  virtual void visit_target_register (target_register*) {};
  virtual void visit_target_deref (target_deref*) {};
  virtual void visit_target_bitfield (target_bitfield*) {};
  virtual void visit_target_symbol (target_symbol*) {};
  virtual void visit_arrayindex (arrayindex*) {};
  virtual void visit_functioncall (functioncall*) {};
  virtual void visit_print_format (print_format*) {};
  virtual void visit_stat_op (stat_op*) {};
  virtual void visit_hist_op (hist_op*) {};
  virtual void visit_cast_op (cast_op*) {};
  virtual void visit_autocast_op (autocast_op*) {};
  virtual void visit_atvar_op (atvar_op*) {};
  virtual void visit_defined_op (defined_op*) {};
  virtual void visit_entry_op (entry_op*) {};
  virtual void visit_perf_op (perf_op*) {};
};


// A simple kind of visitor, which travels down to the leaves of the
// statement/expression tree, up to but excluding following vardecls
// and functioncalls.
struct traversing_visitor: public visitor
{
  void visit_block (block *s);
  void visit_try_block (try_block *s);
  void visit_embeddedcode (embeddedcode *s);
  void visit_null_statement (null_statement *s);
  void visit_expr_statement (expr_statement *s);
  void visit_if_statement (if_statement* s);
  void visit_for_loop (for_loop* s);
  void visit_foreach_loop (foreach_loop* s);
  void visit_return_statement (return_statement* s);
  void visit_delete_statement (delete_statement* s);
  void visit_next_statement (next_statement* s);
  void visit_break_statement (break_statement* s);
  void visit_continue_statement (continue_statement* s);
  void visit_literal_string (literal_string* e);
  void visit_literal_number (literal_number* e);
  void visit_embedded_expr (embedded_expr* e);
  void visit_binary_expression (binary_expression* e);
  void visit_unary_expression (unary_expression* e);
  void visit_pre_crement (pre_crement* e);
  void visit_post_crement (post_crement* e);
  void visit_logical_or_expr (logical_or_expr* e);
  void visit_logical_and_expr (logical_and_expr* e);
  void visit_array_in (array_in* e);
  void visit_regex_query (regex_query* e);
  void visit_compound_expression(compound_expression* e);
  void visit_comparison (comparison* e);
  void visit_concatenation (concatenation* e);
  void visit_ternary_expression (ternary_expression* e);
  void visit_assignment (assignment* e);
  void visit_symbol (symbol* e);
  void visit_target_register (target_register* e);
  void visit_target_deref (target_deref* e);
  void visit_target_bitfield (target_bitfield* e);
  void visit_target_symbol (target_symbol* e);
  void visit_arrayindex (arrayindex* e);
  void visit_functioncall (functioncall* e);
  void visit_print_format (print_format* e);
  void visit_stat_op (stat_op* e);
  void visit_hist_op (hist_op* e);
  void visit_cast_op (cast_op* e);
  void visit_autocast_op (autocast_op* e);
  void visit_atvar_op (atvar_op* e);
  void visit_defined_op (defined_op* e);
  void visit_entry_op (entry_op* e);
  void visit_perf_op (perf_op* e);
};


// A visitor that calls a generic visit_expression on every expression.
struct expression_visitor: public traversing_visitor
{
  virtual void visit_expression(expression *e) = 0;

  void visit_literal_string (literal_string* e);
  void visit_literal_number (literal_number* e);
  void visit_embedded_expr (embedded_expr* e);
  void visit_binary_expression (binary_expression* e);
  void visit_unary_expression (unary_expression* e);
  void visit_pre_crement (pre_crement* e);
  void visit_post_crement (post_crement* e);
  void visit_logical_or_expr (logical_or_expr* e);
  void visit_logical_and_expr (logical_and_expr* e);
  void visit_array_in (array_in* e);
  void visit_regex_query (regex_query* e);
  void visit_compound_expression (compound_expression* e);
  void visit_comparison (comparison* e);
  void visit_concatenation (concatenation* e);
  void visit_ternary_expression (ternary_expression* e);
  void visit_assignment (assignment* e);
  void visit_symbol (symbol* e);
  void visit_target_register (target_register* e);
  void visit_target_deref (target_deref* e);
  void visit_target_bitfield (target_bitfield* e);
  void visit_target_symbol (target_symbol* e);
  void visit_arrayindex (arrayindex* e);
  void visit_functioncall (functioncall* e);
  void visit_print_format (print_format* e);
  void visit_stat_op (stat_op* e);
  void visit_hist_op (hist_op* e);
  void visit_cast_op (cast_op* e);
  void visit_autocast_op (autocast_op* e);
  void visit_atvar_op (atvar_op* e);
  void visit_defined_op (defined_op* e);
  void visit_entry_op (entry_op* e);
  void visit_perf_op (perf_op* e);
};


// A kind of traversing visitor, which also follows function calls.
// It uses an internal set object to prevent infinite recursion.
struct functioncall_traversing_visitor: public traversing_visitor
{
  std::set<functiondecl*> seen;
  std::set<functiondecl*> nested;
  functiondecl* current_function;
  functioncall_traversing_visitor(): current_function(0) {}
  void visit_functioncall (functioncall* e);
  void enter_functioncall (functioncall* e);
  virtual void note_recursive_functioncall (functioncall* e);
};


// A kind of traversing visitor, which also follows function calls,
// and stores the vardecl* referent of each variable read and/or
// written and other such sundry side-effect data.  It's used by
// the elaboration-time optimizer pass.
struct varuse_collecting_visitor: public functioncall_traversing_visitor
{
  systemtap_session& session;
  std::set<vardecl*> read;
  std::set<vardecl*> written;
  std::set<vardecl*> used;
  bool embedded_seen;
  bool current_lvalue_read;
  expression* current_lvalue;
  expression* current_lrvalue;
  varuse_collecting_visitor(systemtap_session& s):
    session (s),
    embedded_seen (false),
    current_lvalue_read (false),
    current_lvalue(0),
    current_lrvalue(0) {}
  void visit_if_statement (if_statement* s);
  void visit_for_loop (for_loop* s);
  void visit_embeddedcode (embeddedcode *s);
  void visit_embedded_expr (embedded_expr *e);
  void visit_try_block (try_block *s);
  void visit_functioncall (functioncall* e);
  void visit_return_statement (return_statement *s);
  void visit_delete_statement (delete_statement *s);
  void visit_print_format (print_format *e);
  void visit_assignment (assignment *e);
  void visit_ternary_expression (ternary_expression* e);
  void visit_arrayindex (arrayindex *e);
  void visit_target_register (target_register* e);
  void visit_target_deref (target_deref* e);
  void visit_target_symbol (target_symbol *e);
  void visit_symbol (symbol *e);
  void visit_pre_crement (pre_crement *e);
  void visit_post_crement (post_crement *e);
  void visit_foreach_loop (foreach_loop *s);
  void visit_cast_op (cast_op* e);
  void visit_autocast_op (autocast_op* e);
  void visit_atvar_op (atvar_op *e);
  void visit_defined_op (defined_op* e);
  void visit_entry_op (entry_op* e);
  void visit_perf_op (perf_op* e);
  bool side_effect_free ();
  bool side_effect_free_wrt (const std::set<vardecl*>& vars);
};



// A kind of visitor that throws an semantic_error exception
// whenever a non-overridden method is called.
struct throwing_visitor: public visitor
{
  std::string msg;
  throwing_visitor (const std::string& m);
  throwing_visitor ();

  virtual void throwone (const token* t);

  void visit_block (block *s);
  void visit_try_block (try_block *s);
  void visit_embeddedcode (embeddedcode *s);
  void visit_null_statement (null_statement *s);
  void visit_expr_statement (expr_statement *s);
  void visit_if_statement (if_statement* s);
  void visit_for_loop (for_loop* s);
  void visit_foreach_loop (foreach_loop* s);
  void visit_return_statement (return_statement* s);
  void visit_delete_statement (delete_statement* s);
  void visit_next_statement (next_statement* s);
  void visit_break_statement (break_statement* s);
  void visit_continue_statement (continue_statement* s);
  void visit_literal_string (literal_string* e);
  void visit_literal_number (literal_number* e);
  void visit_embedded_expr (embedded_expr* e);
  void visit_binary_expression (binary_expression* e);
  void visit_unary_expression (unary_expression* e);
  void visit_pre_crement (pre_crement* e);
  void visit_post_crement (post_crement* e);
  void visit_logical_or_expr (logical_or_expr* e);
  void visit_logical_and_expr (logical_and_expr* e);
  void visit_array_in (array_in* e);
  void visit_regex_query (regex_query* e);
  void visit_compound_expression (compound_expression* e);
  void visit_comparison (comparison* e);
  void visit_concatenation (concatenation* e);
  void visit_ternary_expression (ternary_expression* e);
  void visit_assignment (assignment* e);
  void visit_symbol (symbol* e);
  void visit_target_register (target_register* e);
  void visit_target_deref (target_deref* e);
  void visit_target_bitfield (target_bitfield* e);
  void visit_target_symbol (target_symbol* e);
  void visit_arrayindex (arrayindex* e);
  void visit_functioncall (functioncall* e);
  void visit_print_format (print_format* e);
  void visit_stat_op (stat_op* e);
  void visit_hist_op (hist_op* e);
  void visit_cast_op (cast_op* e);
  void visit_autocast_op (autocast_op* e);
  void visit_atvar_op (atvar_op* e);
  void visit_defined_op (defined_op* e);
  void visit_entry_op (entry_op* e);
  void visit_perf_op (perf_op* e);
};

// A visitor similar to a traversing_visitor, but with the ability to rewrite
// parts of the tree through require/provide.
//
// The relaxed_p member variable is cleared if an update occurred.
//
struct update_visitor: public visitor
{
  template <typename T> T* require (T* src, bool clearok=false)
  {
    T* dst = NULL;
    if (src != NULL)
      {
        src->visit(this);
        if (values.empty())
          throw std::runtime_error(_("update_visitor wasn't provided a value"));
        visitable *v = values.top();
        values.pop();
        if (v == NULL && !clearok)
          throw std::runtime_error(_("update_visitor was provided a NULL value"));
        dst = dynamic_cast<T*>(v);
        if (v != NULL && dst == NULL)
          throw std::runtime_error(_F("update_visitor can't set type \"%s\" with a \"%s\"",
                                      typeid(T).name(), typeid(*v).name()));
      }
    return dst;
  }

  template <typename T> void provide (T* src)
  {
    values.push(src);
  }

  template <typename T> void abort_provide (T* src)
  {
    values.push(src);
    aborted_p = true;
  }

  template <typename T> void replace (T*& src, bool clearok=false)
  {
    if (! aborted_p)
      {
        const T* old_src = src;
        T* new_src = require(src, clearok);
        if (old_src != new_src)
          {
	    if (this->verbose > 3)
	      {
		std::clog << _("replaced ");
		old_src->print(std::clog);
		std::clog << _(" with ");
		new_src->print(std::clog);
		std::clog << std::endl;
	      }
            relaxed_p = false;
          }
        src = new_src;
      }
  }

  update_visitor(unsigned v = 0): verbose(v), aborted_p(false), relaxed_p(true) {}
  virtual ~update_visitor() { assert(values.empty()); }

  // Permit reuse of the visitor object. 
  virtual void reset() { aborted_p = false; relaxed_p = true; }
  virtual bool relaxed() { return relaxed_p; }
    
  virtual void visit_block (block *s);
  virtual void visit_try_block (try_block *s);
  virtual void visit_embeddedcode (embeddedcode *s);
  virtual void visit_null_statement (null_statement *s);
  virtual void visit_expr_statement (expr_statement *s);
  virtual void visit_if_statement (if_statement* s);
  virtual void visit_for_loop (for_loop* s);
  virtual void visit_foreach_loop (foreach_loop* s);
  virtual void visit_return_statement (return_statement* s);
  virtual void visit_delete_statement (delete_statement* s);
  virtual void visit_next_statement (next_statement* s);
  virtual void visit_break_statement (break_statement* s);
  virtual void visit_continue_statement (continue_statement* s);
  virtual void visit_literal_string (literal_string* e);
  virtual void visit_literal_number (literal_number* e);
  virtual void visit_embedded_expr (embedded_expr* e);
  virtual void visit_binary_expression (binary_expression* e);
  virtual void visit_unary_expression (unary_expression* e);
  virtual void visit_pre_crement (pre_crement* e);
  virtual void visit_post_crement (post_crement* e);
  virtual void visit_logical_or_expr (logical_or_expr* e);
  virtual void visit_logical_and_expr (logical_and_expr* e);
  virtual void visit_array_in (array_in* e);
  virtual void visit_regex_query (regex_query* e);
  virtual void visit_compound_expression (compound_expression* e);
  virtual void visit_comparison (comparison* e);
  virtual void visit_concatenation (concatenation* e);
  virtual void visit_ternary_expression (ternary_expression* e);
  virtual void visit_assignment (assignment* e);
  virtual void visit_symbol (symbol* e);
  virtual void visit_target_register (target_register* e);
  virtual void visit_target_deref (target_deref* e);
  virtual void visit_target_bitfield (target_bitfield* e);
  virtual void visit_target_symbol (target_symbol* e);
  virtual void visit_arrayindex (arrayindex* e);
  virtual void visit_functioncall (functioncall* e);
  virtual void visit_print_format (print_format* e);
  virtual void visit_stat_op (stat_op* e);
  virtual void visit_hist_op (hist_op* e);
  virtual void visit_cast_op (cast_op* e);
  virtual void visit_autocast_op (autocast_op* e);
  virtual void visit_atvar_op (atvar_op* e);
  virtual void visit_defined_op (defined_op* e);
  virtual void visit_entry_op (entry_op* e);
  virtual void visit_perf_op (perf_op* e);

private:
  std::stack<visitable *> values;
protected:
  unsigned verbose;
  bool aborted_p;
  bool relaxed_p;
};

// A visitor which performs a deep copy of the root node it's applied
// to. NB: It does not copy any of the variable or function
// declarations; those fields are set to NULL, assuming you want to
// re-infer the declarations in a new context (the one you're copying
// to).

struct deep_copy_visitor: public update_visitor
{
  template <typename T> static T* deep_copy (T* e)
  {
    deep_copy_visitor v;
    return v.require (e);
  }

  virtual void visit_block (block *s);
  virtual void visit_try_block (try_block *s);
  virtual void visit_embeddedcode (embeddedcode *s);
  virtual void visit_null_statement (null_statement *s);
  virtual void visit_expr_statement (expr_statement *s);
  virtual void visit_if_statement (if_statement* s);
  virtual void visit_for_loop (for_loop* s);
  virtual void visit_foreach_loop (foreach_loop* s);
  virtual void visit_return_statement (return_statement* s);
  virtual void visit_delete_statement (delete_statement* s);
  virtual void visit_next_statement (next_statement* s);
  virtual void visit_break_statement (break_statement* s);
  virtual void visit_continue_statement (continue_statement* s);
  virtual void visit_literal_string (literal_string* e);
  virtual void visit_literal_number (literal_number* e);
  virtual void visit_embedded_expr (embedded_expr* e);
  virtual void visit_binary_expression (binary_expression* e);
  virtual void visit_unary_expression (unary_expression* e);
  virtual void visit_pre_crement (pre_crement* e);
  virtual void visit_post_crement (post_crement* e);
  virtual void visit_logical_or_expr (logical_or_expr* e);
  virtual void visit_logical_and_expr (logical_and_expr* e);
  virtual void visit_array_in (array_in* e);
  virtual void visit_regex_query (regex_query* e);
  virtual void visit_compound_expression(compound_expression* e);
  virtual void visit_comparison (comparison* e);
  virtual void visit_concatenation (concatenation* e);
  virtual void visit_ternary_expression (ternary_expression* e);
  virtual void visit_assignment (assignment* e);
  virtual void visit_symbol (symbol* e);
  virtual void visit_target_register (target_register* e);
  virtual void visit_target_deref (target_deref* e);
  virtual void visit_target_bitfield (target_bitfield* e);
  virtual void visit_target_symbol (target_symbol* e);
  virtual void visit_arrayindex (arrayindex* e);
  virtual void visit_functioncall (functioncall* e);
  virtual void visit_print_format (print_format* e);
  virtual void visit_stat_op (stat_op* e);
  virtual void visit_hist_op (hist_op* e);
  virtual void visit_cast_op (cast_op* e);
  virtual void visit_autocast_op (autocast_op* e);
  virtual void visit_atvar_op (atvar_op* e);
  virtual void visit_defined_op (defined_op* e);
  virtual void visit_entry_op (entry_op* e);
  virtual void visit_perf_op (perf_op* e);
};

struct embedded_tags_visitor: public traversing_visitor
{
  std::set<interned_string> available_tags;
  std::set<interned_string> tags; // set of the tags that appear in the code
  embedded_tags_visitor(bool all_tags);
  void visit_embeddedcode (embeddedcode *s);
  void visit_embedded_expr (embedded_expr *e);
};

#endif // STAPTREE_H

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
