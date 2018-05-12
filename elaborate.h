// -*- C++ -*-
// Copyright (C) 2005-2017 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#ifndef ELABORATE_H
#define ELABORATE_H

#include "staptree.h"
#include "parse.h"
#include "stringtable.h"
#include "session.h"

#include <string>
#include <vector>
//#include <iostream>
#include <iosfwd>
#include <sstream>
#include <map>
#include <list>

extern "C" {
#include <elfutils/libdw.h>
}

#include "privilege.h"

struct recursive_expansion_error : public semantic_error
{
  ~recursive_expansion_error () throw () {}
  recursive_expansion_error (const std::string& msg, const token* t1=0):
    SEMANTIC_ERROR (msg, t1) {}

  recursive_expansion_error (const std::string& msg, const token* t1,
                             const token* t2):
    SEMANTIC_ERROR (msg, t1, t2) {}
};

// ------------------------------------------------------------------------

struct derived_probe;
class match_node;

struct symresolution_info: public traversing_visitor
{
protected:
  systemtap_session& session;
  bool unmangled_p;
public:
  functiondecl* current_function;
  derived_probe* current_probe;
  symresolution_info (systemtap_session& s, bool omniscient_unmangled = false);

  vardecl* find_var (interned_string name, int arity, const token *tok);
  std::vector<functiondecl*> find_functions (const std::string& name, unsigned arity, const token *tok);
  std::set<std::string> collect_functions(void);

  void visit_block (block *s);
  void visit_symbol (symbol* e);
  void visit_foreach_loop (foreach_loop* e);
  void visit_arrayindex (arrayindex* e);
  void visit_arrayindex (arrayindex *e, bool wildcard_ok);
  void visit_functioncall (functioncall* e);
  void visit_delete_statement (delete_statement* s);
  void visit_array_in (array_in *e);
};


struct typeresolution_info: public visitor
{
  typeresolution_info (systemtap_session& s);
  systemtap_session& session;
  unsigned num_newly_resolved;
  unsigned num_still_unresolved;
  unsigned num_available_autocasts;
  bool assert_resolvability;
  int mismatch_complexity;
  functiondecl* current_function;
  derived_probe* current_probe;

  // Holds information about a type we resolved (see PR16097)
  struct resolved_type
  {
    const token *tok;
    const symboldecl *decl;
    int index;
    resolved_type(const token *ct, const symboldecl *cdecl, int cindex):
      tok(ct), decl(cdecl), index(cindex) {}
  };

  // Holds an element each time we resolve a decl. Unique by decl & index.
  // Possible values:
  //  - resolved function type     -> decl = functiondecl, index = -1
  //  - resolved function arg type -> decl = vardecl,      index = index of arg
  //  - resolved array/var type    -> decl = vardecl,      index = -1
  //  - resolved array index type  -> decl = vardecl,      index = index of type
  std::vector<resolved_type> resolved_types; // see PR16097

  void check_arg_type (exp_type wanted, expression* arg);
  void check_local (vardecl* v);
  void unresolved (const token* tok);
  void invalid (const token* tok, exp_type t);
  void mismatch (const binary_expression* e);
  void mismatch (const token* tok, exp_type t1, exp_type t2);
  void mismatch (const token* tok, exp_type type,
                 const symboldecl* decl, int index = -1);
  void resolved (const token* tok, exp_type type,
                 const symboldecl* decl = NULL, int index = -1);
  void resolved_details (const exp_type_ptr& src, exp_type_ptr& dest);

  exp_type t; // implicit parameter for nested visit call; may clobber
              // Upon entry to one of the visit_* calls, the incoming
              // `t' value is the type inferred for that node from 
              // context.  It may match or conflict with the node's 
              // preexisting type, or it may be unknown.

  // Expressions with NULL type_details may be as-yet-unknown.
  // If they have this null_type, they're explicitly *not* a rich type.
  const exp_type_ptr null_type;

  void visit_block (block* s);
  void visit_try_block (try_block* s);
  void visit_embeddedcode (embeddedcode* s);
  void visit_null_statement (null_statement* s);
  void visit_expr_statement (expr_statement* s);
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


// ------------------------------------------------------------------------

// A derived_probe is a probe that has been elaborated by
// binding to a matching provider.  The locations std::vector
// may be smaller or larger than the base probe, since a
// provider may transform it.

class translator_output;
struct derived_probe_group;

struct derived_probe: public probe
{
  derived_probe (probe* b, probe_point* l, bool rewrite_loc=false);
  probe* base; // the original parsed probe
  probe_point* base_pp; // the probe_point that led to this derivation
  derived_probe_group* group; // the group we belong to
  virtual ~derived_probe () {}
  virtual void join_group (systemtap_session& s) = 0;
  virtual probe_point* sole_location () const;
  virtual probe_point* script_location () const;
  virtual void printsig (std::ostream &o) const;
  // return arguments of probe if there
  virtual void getargs (std::list<std::string> &) const {}
  void printsig_nested (std::ostream &o) const;
  virtual void collect_derivation_chain (std::vector<probe*> &probes_list) const;
  virtual void collect_derivation_pp_chain (std::vector<probe_point*> &pp_list) const;
  std::string derived_locations (bool firstFrom = true);

  virtual void print_dupe_stamp(std::ostream&) {}
  // To aid duplication elimination, print a stamp which uniquely identifies
  // the code that will be added to the probe body.  (Doesn't need to be the
  // actual code...)

  virtual void initialize_probe_context_vars (translator_output*) {}
  // From within unparser::emit_probe, initialized any extra variables
  // in this probe's context locals.

  virtual void emit_probe_local_init (systemtap_session&, translator_output*) {}
  // From within unparser::emit_probe, emit any extra processing block
  // for this probe.

  virtual void emit_privilege_assertion (translator_output*);
  // From within unparser::emit_probe, emit any unprivileged mode
  // checking for this probe.

public:
  static void emit_common_header (translator_output* o);
  // from c_unparser::emit_common_header
  // XXX: probably can move this stuff to a probe_group::emit_module_decls

  static void emit_process_owner_assertion (translator_output*);
  // From within unparser::emit_probe, emit a check that the current
  // process belongs to the user.

  static void print_dupe_stamp_unprivileged(std::ostream& o);
  static void print_dupe_stamp_unprivileged_process_owner(std::ostream& o);

  virtual bool needs_global_locks () { return true; }
  // by default, probes need locks around global variables

  // Location of semaphores to activate sdt probes
  Dwarf_Addr sdt_semaphore_addr;

  // perf.counter probes that this probe references
  std::set<std::string> perf_counter_refs;

  // index into session.probes[], set and used during translation
  unsigned session_index;

  // List of other derived probes whose conditions may be affected by
  // this probe.
  std::set<derived_probe*> probes_with_affected_conditions;

  virtual void use_internal_buffer(const std::string&) {}
};

// ------------------------------------------------------------------------

struct unparser;

// Various derived classes derived_probe_group manage the
// registration/invocation/unregistration of sibling probes.
struct derived_probe_group
{
  virtual ~derived_probe_group () {}

  virtual void emit_kernel_module_init (systemtap_session&) {}
  // Similar to emit_module_init(), but code emitted here gets run
  // with root access.  The _init-generated code may assume that it is
  // called only once.  If that code fails at run time, it must set
  // rc=1 and roll back any partial initializations, for its _exit
  // friend will NOT be invoked.  The generated code may use
  // pre-declared "int i, j;".  Note that the message transport isn't
  // available, so printk()/errk() is the only output option.

  virtual void emit_kernel_module_exit (systemtap_session&) {}
  // Similar to emit_module_exit(), but code emitted here gets run
  // with root access.  The _exit-generated code may assume that it is
  // executed exactly zero times (if the _init-generated code failed)
  // or once.  (_exit itself may be called a few times, to generate
  // the code in a few different places in the probe module.)  The
  // generated code may use pre-declared "int i, j;".  Note that the
  // message transport isn't available, so printk()/errk() is the only
  // output option.

  virtual void emit_module_decls (systemtap_session& s) = 0;
  // The _decls-generated code may assume that declarations such as
  // the context, embedded-C code, function and probe handler bodies
  // are all already generated.  That is, _decls is called near the
  // end of the code generation process.  It should minimize the
  // number of separate variables (and to a lesser extent, their
  // size).

  virtual void emit_module_init (systemtap_session& s) = 0;
  // The _init-generated code may assume that it is called only once.
  // If that code fails at run time, it must set rc=1 and roll back
  // any partial initializations, for its _exit friend will NOT be
  // invoked.  The generated code may use pre-declared "int i, j;"
  // and set "const char* probe_point;".

  virtual void emit_module_post_init (systemtap_session&) {}
  // The emit_module_post_init() code is called once session_state is
  // set to running.

  virtual void emit_module_refresh (systemtap_session&) {}
  // The _refresh-generated code may be called multiple times during
  // a session run, bracketed by _init and _exit calls.
  // Upon failure, it must set enough state so that
  // a subsequent _exit call will clean up everything.
  // The generated code may use pre-declared "int i, j;".

  virtual void emit_module_exit (systemtap_session& s) = 0;
  // The _exit-generated code may assume that it is executed exactly
  // zero times (if the _init-generated code failed) or once.  (_exit
  // itself may be called a few times, to generate the code in a few
  // different places in the probe module.)
  // The generated code may use pre-declared "int i, j;".

  // Support for on-the-fly operations is implemented in the runtime using a
  // workqueue which calls module_refresh(). Depending on the probe type, it may
  // not be safe to manipulate the workqueue in the context of the probe handler
  // (otf_safe_context() = false). In this case, we rely on a background timer
  // to schedule the work. Otherwise, if the probe context is safe
  // (otf_safe_context() = true), we can directly schedule the work.

  virtual bool otf_supported (systemtap_session&) { return false; }
  // Support for on-the-fly arming/disarming depends on probe type

  virtual bool otf_safe_context (systemtap_session&) { return false; }
  // Whether this probe type occurs in a safe context. To be safe, we default to
  // no, which means we'll rely on a background timer.
};


// ------------------------------------------------------------------------

typedef std::map<interned_string, literal*> literal_map_t;

struct derived_probe_builder
{
  virtual void build(systemtap_session & sess,
		     probe* base,
		     probe_point* location,
		     literal_map_t const & parameters,
		     std::vector<derived_probe*> & finished_results) = 0;
  virtual void build_with_suffix(systemtap_session & sess,
                                 probe * use,
                                 probe_point * location,
                                 literal_map_t const & parameters,
                                 std::vector<derived_probe *>
                                   & finished_results,
                                 std::vector<probe_point::component *>
                                   const & suffix);
  virtual ~derived_probe_builder() {}
  virtual void build_no_more (systemtap_session &) {}
  virtual bool is_alias () const { return false; }
  virtual std::string name() = 0;

  static bool has_null_param (literal_map_t const & parameters,
                              interned_string key);
  static bool get_param (literal_map_t const & parameters,
                         interned_string key, interned_string& value);
  static bool get_param (literal_map_t const & parameters,
                         interned_string key, int64_t& value);
  static bool has_param (literal_map_t const & parameters,
                         interned_string key);
};


struct
match_key
{
  interned_string name;
  bool have_parameter;
  exp_type parameter_type;

  match_key(interned_string n);
  match_key(probe_point::component const & c);

  match_key & with_number();
  match_key & with_string();
  std::string str() const;
  bool operator<(match_key const & other) const;
  bool globmatch(match_key const & other) const;
};


class
match_node
{
  typedef std::map<match_key, match_node*> sub_map_t;
  typedef std::map<match_key, match_node*>::iterator sub_map_iterator_t;
  sub_map_t sub;
  std::vector<derived_probe_builder*> ends;

 public:
  match_node();

  void find_and_build (systemtap_session& s,
                       probe* p, probe_point *loc, unsigned pos,
                       std::vector<derived_probe *>& results,
                       std::set<std::string>& builders);
  std::string suggest_functors(systemtap_session& s, std::string functor);
  void try_suffix_expansion (systemtap_session& s,
                             probe *p, probe_point *loc, unsigned pos,
                             std::vector<derived_probe *>& results);
  void build_no_more (systemtap_session &s);
  void dump (systemtap_session &s, const std::string &name = "");

  match_node* bind(match_key const & k);
  match_node* bind(interned_string k);
  match_node* bind_str(std::string const & k);
  match_node* bind_num(std::string const & k);
  match_node* bind_privilege(privilege_t p = privilege_t (pr_stapdev | pr_stapsys));
  void bind(derived_probe_builder* e);

private:
  privilege_t privilege;
};

// ------------------------------------------------------------------------

struct
alias_expansion_builder
  : public derived_probe_builder
{
  probe_alias * alias;

  alias_expansion_builder(probe_alias * a)
    : alias(a)
  {}

  virtual void build(systemtap_session & sess,
		     probe * use,
		     probe_point * location,
		     literal_map_t const &,
		     std::vector<derived_probe *> & finished_results);
  virtual void build_with_suffix(systemtap_session & sess,
                                 probe * use,
                                 probe_point * location,
                                 literal_map_t const &,
                                 std::vector<derived_probe *>
                                   & finished_results,
                                 std::vector<probe_point::component *>
                                   const & suffix);
  virtual bool is_alias () const { return true; }
  virtual std::string name() { return "alias expansion builder"; }

  bool checkForRecursiveExpansion (probe *use);
};

// ------------------------------------------------------------------------

/* struct systemtap_session moved to session.h */

int semantic_pass (systemtap_session& s);
void derive_probes (systemtap_session& s,
                    probe *p, std::vector<derived_probe*>& dps,
                    bool optional = false, bool rethrow_errors = false);

// A helper we use here and in translate, for pulling symbols out of lvalue
// expressions.
symbol * get_symbol_within_expression (expression *e);

struct unparser;


struct const_folder: public update_visitor
{
  systemtap_session& session;
  bool& relaxed_p;
  bool collapse_defines_p;
  
  const_folder(systemtap_session& s, bool& r, bool collapse_defines = false):
    update_visitor(s.verbose), session(s), relaxed_p(r), collapse_defines_p(collapse_defines),
    last_number(0), last_string(0), last_target_symbol(0) {}

  literal_number* last_number;
  literal_number* get_number(expression*& e);
  void visit_literal_number (literal_number* e);

  literal_string* last_string;
  literal_string* get_string(expression*& e);
  void visit_literal_string (literal_string* e);

  void get_literal(expression*& e, literal_number*& n, literal_string*& s);

  void visit_if_statement (if_statement* s);
  void visit_for_loop (for_loop* s);
  void visit_foreach_loop (foreach_loop* s);
  void visit_binary_expression (binary_expression* e);
  void visit_unary_expression (unary_expression* e);
  void visit_logical_or_expr (logical_or_expr* e);
  void visit_logical_and_expr (logical_and_expr* e);
  void visit_compound_expression (compound_expression* e);
  // void visit_regex_query (regex_query* e); // XXX: would require executing dfa at compile-time
  void visit_comparison (comparison* e);
  void visit_concatenation (concatenation* e);
  void visit_ternary_expression (ternary_expression* e);
  void visit_defined_op (defined_op* e);

  target_symbol* last_target_symbol;
  target_symbol* get_target_symbol(expression*& e);
  void visit_target_symbol (target_symbol* e);
};



// Run the given code filter visitors against the given body.
// Repeat until they all report having relaxed.
template <class T>
void update_visitor_loop (systemtap_session& sess, std::vector<update_visitor*>& filters, T& body)
{
  bool relaxed_p;
  do
    {
      relaxed_p = true;
      for (unsigned k=0; k<filters.size(); k++)
        {
          filters[k]->reset ();
          filters[k]->replace (body);
          relaxed_p = (relaxed_p && filters[k]->relaxed());
        }
      if (! relaxed_p && sess.verbose > 3)
        std::clog << _("Rerunning the code filters.") << std::endl;
    } while (! relaxed_p);
}


// Run given code filter visitor, then a round of const folder, over and over, until they chill.
template <class X, class Y>
void var_expand_const_fold_loop(systemtap_session& sess, X& body, Y& v)
{
  bool relaxed_p; /* ignored */
  const_folder cf (sess, relaxed_p);
  std::vector<update_visitor*> k;
  k.push_back (& v);
  k.push_back (& cf);
  update_visitor_loop (sess, k, body);
}



#endif // ELABORATE_H

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
