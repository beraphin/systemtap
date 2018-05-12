// recursive descent parser for systemtap scripts
// Copyright (C) 2005-2016 Red Hat Inc.
// Copyright (C) 2006 Intel Corporation.
// Copyright (C) 2007 Bull S.A.S
// Copyright (C) 2014 Peter Kjellstrom <cap@nsc.liu.se>
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "parse.h"
#include "session.h"
#include "util.h"
#include "stringtable.h"

#include <iostream>

#include <fstream>
#include <cctype>
#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <climits>
#include <sstream>
#include <cstring>
#include <cctype>
#include <iterator>
#include <unordered_set>

extern "C" {
#include <fnmatch.h>
}

using namespace std;


class parser;
class lexer
{
public:
  bool ate_comment; // current token follows a comment
  bool ate_whitespace; // the most recent token followed whitespace
  bool saw_tokens; // the lexer found tokens (before preprocessing occurred)
  bool check_compatible; // whether to gate features on session.compatible

  token* scan ();
  lexer (istream&, const string&, systemtap_session&, bool);
  void set_current_file (stapfile* f);
  void set_current_token_chain (const token* tok);
  inline bool has_version (const char* v) const;

  unordered_set<interned_string> keywords;
  static unordered_set<string> atwords;
private:
  inline int input_get ();
  inline int input_peek (unsigned n=0);
  void input_put (const string&, const token*);
  string input_name;
  string input_contents; // NB: being a temporary, no need to interned_string optimize this object
  const char *input_pointer; // index into input_contents; NB: recompute if input_contents changed!
  const char *input_end;
  unsigned cursor_suspend_count;
  unsigned cursor_suspend_line;
  unsigned cursor_suspend_column;
  unsigned cursor_line;
  unsigned cursor_column;
  systemtap_session& session;
  stapfile* current_file;
  const token* current_token_chain;
};


class parser
{
public:
  parser (systemtap_session& s, const string& n, istream& i, unsigned flags=0);
  ~parser ();

  stapfile* parse ();
  probe* parse_synthetic_probe (const token* chain);
  stapfile* parse_library_macros ();

private:
  typedef enum {
      PP_NONE,
      PP_KEEP_THEN,
      PP_SKIP_THEN,
      PP_KEEP_ELSE,
      PP_SKIP_ELSE,
  } pp_state_t;

  struct pp1_activation;

  struct pp_macrodecl : public macrodecl {
    pp1_activation* parent_act; // used for param bindings
    virtual bool is_closure() { return parent_act != 0; }
    pp_macrodecl () : macrodecl(), parent_act(0) { }
  };

  systemtap_session& session;
  string input_name;
  lexer input;
  bool errs_as_warnings;
  bool privileged;
  bool user_file;
  bool auto_path;
  parse_context context;

  // preprocessing subordinate, first pass (macros)
  struct pp1_activation {
    const token* tok;
    unsigned cursor; // position within macro body
    map<string, pp_macrodecl*> params;

    macrodecl* curr_macro;

    pp1_activation (const token* tok, macrodecl* curr_macro)
      : tok(tok), cursor(0), curr_macro(curr_macro) { }
    ~pp1_activation ();
  };

  map<string, macrodecl*> pp1_namespace;
  vector<pp1_activation*> pp1_state;
  const token* next_pp1 ();
  const token* scan_pp1 (bool ignore_macros);
  const token* slurp_pp1_param (vector<const token*>& param);
  const token* slurp_pp1_body (vector<const token*>& body);

  // preprocessing subordinate, final pass (conditionals)
  vector<pair<const token*, pp_state_t> > pp_state;
  const token* scan_pp ();
  const token* skip_pp ();

  // scanning state
  const token* next ();
  const token* peek ();

  // Advance past and throw away current token after peek () or next ().
  void swallow ();

  const token* systemtap_v_seen;
  const token* last_t; // the last value returned by peek() or next()
  const token* next_t; // lookahead token

  // expectations, these swallow the token
  void expect_known (token_type tt, string const & expected);
  void expect_unknown (token_type tt, interned_string & target);
  void expect_unknown2 (token_type tt1, token_type tt2, interned_string & target);

  // convenience forms, these also swallow the token
  void expect_op (string const & expected);
  interned_string expect_op_any (initializer_list<const char*> expected);
  void expect_kw (string const & expected);
  void expect_number (int64_t & expected);
  void expect_ident_or_keyword (interned_string & target);

  // convenience forms, which return true or false, these don't swallow token
  bool peek_op (string const & op);
  bool peek_kw (string const & kw);

  // convenience forms, which return the token
  const token* expect_kw_token (string const & expected);
  const token* expect_ident_or_atword (interned_string & target);

  void print_error (const parse_error& pe, bool errs_as_warnings = false);
  unsigned num_errors;

private: // nonterminals
  void parse_probe (vector<probe*>&, vector<probe_alias*>&);
  void parse_private (vector<vardecl*>&, vector<probe*>&,
                      string const&, vector<functiondecl*>&);
  void parse_global (vector<vardecl*>&, vector<probe*>&,
                     string const&);
  void do_parse_global (vector<vardecl*>&, vector<probe*>&,
                        string const&, const token*, bool);
  void parse_functiondecl (vector<functiondecl*>&, string const&);
  void do_parse_functiondecl (vector<functiondecl*>&, const token*,
                              string const&, bool);
  embeddedcode* parse_embeddedcode ();
  vector<probe_point*> parse_probe_points ();
  vector<probe_point*> parse_components ();
  vector<probe_point*> parse_component ();
  literal_string* consume_string_literals (const token*);
  literal_string* parse_literal_string ();
  literal* parse_literal ();
  block* parse_stmt_block ();
  try_block* parse_try_block ();
  statement* parse_statement ();
  if_statement* parse_if_statement ();
  for_loop* parse_for_loop ();
  for_loop* parse_while_loop ();
  foreach_loop* parse_foreach_loop ();
  expr_statement* parse_expr_statement ();
  return_statement* parse_return_statement ();
  delete_statement* parse_delete_statement ();
  next_statement* parse_next_statement ();
  break_statement* parse_break_statement ();
  continue_statement* parse_continue_statement ();
  indexable* parse_indexable ();
  const token *parse_hist_op_or_bare_name (hist_op *&hop, interned_string &name);
  target_symbol *parse_target_symbol ();
  cast_op *parse_cast_op ();
  atvar_op *parse_atvar_op ();
  expression* parse_entry_op (const token* t);
  expression* parse_defined_op (const token* t);
  expression* parse_const_op (const token* t);
  expression* parse_perf_op (const token* t);
  expression* parse_expression ();
  expression* parse_assignment ();
  expression* parse_ternary ();
  expression* parse_logical_or ();
  expression* parse_logical_and ();
  expression* parse_boolean_or ();
  expression* parse_boolean_xor ();
  expression* parse_boolean_and ();
  expression* parse_array_in ();
  expression* parse_comparison_or_regex_query ();
  expression* parse_shift ();
  expression* parse_concatenation ();
  expression* parse_additive ();
  expression* parse_multiplicative ();
  expression* parse_unary ();
  expression* parse_crement ();
  expression* parse_dwarf_value ();
  expression* parse_value ();
  expression* parse_symbol ();

  bool peek_target_symbol_components ();
  void parse_target_symbol_components (target_symbol* e);
};


// ------------------------------------------------------------------------

stapfile*
parse (systemtap_session& s, const string& n, istream& i, unsigned flags)
{
  parser p (s, n, i, flags);
  return p.parse ();
}

stapfile*
parse (systemtap_session& s, const string& name, unsigned flags)
{
  ifstream i(name.c_str(), ios::in);
  if (i.fail())
    {
      cerr << (file_exists(name)
               ? _F("Input file '%s' can't be opened for reading.", name.c_str())
               : _F("Input file '%s' is missing.", name.c_str()))
           << endl;
      return 0;
    }

  parser p (s, name, i, flags);
  return p.parse ();
}

stapfile*
parse_library_macros (systemtap_session& s, const string& name)
{
  ifstream i(name.c_str(), ios::in);
  if (i.fail())
    {
      cerr << (file_exists(name)
               ? _F("Input file '%s' can't be opened for reading.", name.c_str())
               : _F("Input file '%s' is missing.", name.c_str()))
           << endl;
      return 0;
    }

  parser p (s, name, i);
  return p.parse_library_macros ();
}

probe*
parse_synthetic_probe (systemtap_session &s, istream& i, const token* tok)
{
  parser p (s, tok ? tok->location.file->name : "<synthetic>", i);
  return p.parse_synthetic_probe (tok);
}

// ------------------------------------------------------------------------

parser::parser (systemtap_session& s, const string &n, istream& i, unsigned flags):
  session (s), input_name (n), input (i, input_name, s, !(flags & pf_no_compatible)),
  errs_as_warnings(flags & pf_squash_errors), privileged (flags & pf_guru),
  user_file (flags & pf_user_file), auto_path (flags & pf_auto_path),
  context(con_unknown), systemtap_v_seen(0), last_t (0), next_t (0), num_errors (0)
{
}

parser::~parser()
{
}

static string
tt2str(token_type tt)
{
  switch (tt)
    {
    case tok_junk: return "junk";
    case tok_identifier: return "identifier";
    case tok_operator: return "operator";
    case tok_string: return "string";
    case tok_number: return "number";
    case tok_embedded: return "embedded-code";
    case tok_keyword: return "keyword";
    }
  return "unknown token";
}

ostream&
operator << (ostream& o, const source_loc& loc)
{
  o << loc.file->name << ":"
    << loc.line << ":"
    << loc.column;

  return o;
}

ostream&
operator << (ostream& o, const token& t)
{
  o << tt2str(t.type);

  if (t.type != tok_embedded && t.type != tok_keyword) // XXX: other types?
    {
      o << " '";
      for (unsigned i=0; i<t.content.length(); i++)
        {
          char c = t.content[i];
          o << (isprint (c) ? c : '?');
        }
      o << "'";
    }

  o << " at "
    << t.location;

  return o;
}


void
parser::print_error  (const parse_error &pe, bool errs_as_warnings)
{
  const token *tok = pe.tok ? pe.tok : last_t;
  session.print_error(pe, tok, input_name, errs_as_warnings);
  num_errors ++;
}




template <typename OPERAND>
bool eval_comparison (const OPERAND& lhs, const token* op, const OPERAND& rhs)
{
  if (op->type == tok_operator && op->content == "<=")
    { return lhs <= rhs; }
  else if (op->type == tok_operator && op->content == ">=")
    { return lhs >= rhs; }
  else if (op->type == tok_operator && op->content == "<")
    { return lhs < rhs; }
  else if (op->type == tok_operator && op->content == ">")
    { return lhs > rhs; }
  else if (op->type == tok_operator && op->content == "==")
    { return lhs == rhs; }
  else if (op->type == tok_operator && op->content == "!=")
    { return lhs != rhs; }
  else
    throw PARSE_ERROR (_("expected comparison operator"), op);
}


// Here, we perform on-the-fly preprocessing in two passes.

// First pass - macro declaration and expansion.
//
// The basic form of a declaration is @define SIGNATURE %( BODY %)
// where SIGNATURE is of the form macro_name (a, b, c, ...)
// and BODY can obtain the parameter contents as @a, @b, @c, ....
// Note that parameterless macros can also be declared.
//
// Macro definitions may not be nested.
// A macro is available textually after it has been defined.
//
// The basic form of a macro invocation
//   for a parameterless macro is @macro_name,
//   for a macro with parameters is @macro_name(param_1, param_2, ...).
//
// NB: this means that a parameterless macro @foo called as @foo(a, b, c)
// leaves its 'parameters' alone, rather than consuming them to result
// in a "too many parameters error". This may be useful in the unusual
// case of wanting @foo to expand to the name of a function.
//
// Invocations of unknown macros are left unexpanded, to allow
// the continued use of constructs such as @cast, @var, etc.

macrodecl::~macrodecl ()
{
  delete tok;
  for (vector<const token*>::iterator it = body.begin();
       it != body.end(); it++)
    delete *it;
}

parser::pp1_activation::~pp1_activation ()
{
  delete tok;
  if (curr_macro->is_closure()) return; // body is shared with an earlier declaration
  for (map<string, pp_macrodecl*>::iterator it = params.begin();
       it != params.end(); it++)
    delete it->second;
}

// Grab a token from the current input source (main file or macro body):
const token*
parser::next_pp1 ()
{
  if (pp1_state.empty())
    return input.scan ();

  // otherwise, we're inside a macro
  pp1_activation* act = pp1_state.back();
  unsigned& cursor = act->cursor;
  if (cursor < act->curr_macro->body.size())
    {
      token* t = new token(*act->curr_macro->body[cursor]);
      t->chain = new token(*act->tok); // mark chained token
      cursor++;
      return t;
    }
  else
    return 0; // reached end of macro body
}

const token*
parser::scan_pp1 (bool ignore_macros = false)
{
  while (true)
    {
      const token* t = next_pp1 ();
      if (t == 0) // EOF or end of macro body
        {
          if (pp1_state.empty()) // actual EOF
            return 0;

          // Exit macro and loop around to look for the next token.
          pp1_activation* act = pp1_state.back();
          pp1_state.pop_back(); delete act;
          continue;
        }

      // macro definition
      // PR18462 don't catalog preprocessor-disabled macros
      if (t->type == tok_operator && t->content == "@define" && !ignore_macros)
        {
          if (!pp1_state.empty())
            throw PARSE_ERROR (_("'@define' forbidden inside macro body"), t);
          delete t;

          // handle macro definition
          // (1) consume macro signature
          t = input.scan();
          if (! (t && t->type == tok_identifier))
            throw PARSE_ERROR (_("expected identifier"), t);
          string name = t->content;

          // check for redefinition of existing macro
          if (pp1_namespace.find(name) != pp1_namespace.end())
            {
              parse_error er (ERR_SRC, _F("attempt to redefine macro '@%s' in the same file", name.c_str ()), t);

              // Also point to pp1_namespace[name]->tok, the site of
              // the original definition:
              er.chain = new PARSE_ERROR (_F("macro '@%s' first defined here",
                                             name.c_str()), pp1_namespace[name]->tok);
              throw er;
            }

          // XXX: the above restriction was mostly necessary due to
          // wanting to leave open the possibility of
          // statically-scoped semantics in the future.

          // XXX: this cascades into further parse errors as the
          // parser tries to parse the remaining definition... (e.g.
          // it can't tell that the macro body isn't a conditional,
          // that the uses of parameters aren't nonexistent
          // macros.....)
          if (name == "define")
            throw PARSE_ERROR (_("attempt to redefine '@define'"), t);
          if (input.atwords.count(name))
            session.print_warning (_F("macro redefines built-in operator '@%s'", name.c_str()), t);

          macrodecl* decl = (pp1_namespace[name] = new macrodecl);
          decl->tok = t;

          // determine if the macro takes parameters
          bool saw_params = false;
          t = input.scan();
          if (t && t->type == tok_operator && t->content == "(")
            {
              saw_params = true;
              do
                {
                  delete t;
                  
                  t = input.scan ();
                  if (! (t && t->type == tok_identifier))
                    throw PARSE_ERROR(_("expected identifier"), t);
                  decl->formal_args.push_back(t->content);
                  delete t;
                  
                  t = input.scan ();
                  if (t && t->type == tok_operator && t->content == ",")
                    {
                      continue;
                    }
                  else if (t && t->type == tok_operator && t->content == ")")
                    {
                      delete t;
                      t = input.scan();
                      break;
                    }
                  else
                    {
                      throw PARSE_ERROR (_("expected ',' or ')'"), t);
                    }
                }
              while (true);
            }

          // (2) identify & consume macro body
          if (! (t && t->type == tok_operator && t->content == "%("))
            {
              if (saw_params)
                throw PARSE_ERROR (_("expected '%('"), t);
              else
                throw PARSE_ERROR (_("expected '%(' or '('"), t);
            }
          delete t;

          t = slurp_pp1_body (decl->body);
          if (!t)
            throw PARSE_ERROR (_("incomplete macro definition - missing '%)'"), decl->tok);
          delete t;

          // Now loop around to look for a real token.
          continue;
        }

      // (potential) macro invocation
      if (t->type == tok_operator && t->content[0] == '@')
        {
          const string& name = t->content.substr(1); // strip initial '@'

          // check if name refers to a real parameter or macro
          macrodecl* decl;
          pp1_activation* act = pp1_state.empty() ? 0 : pp1_state.back();
          if (act && act->params.find(name) != act->params.end())
            decl = act->params[name];
          else if (!(act && act->curr_macro->context == ctx_library)
                   && pp1_namespace.find(name) != pp1_namespace.end())
            decl = pp1_namespace[name];
          else if (session.library_macros.find(name)
                   != session.library_macros.end())
            decl = session.library_macros[name];
          else // this is an ordinary @operator
            return t;

          // handle macro invocation, taking ownership of t
          pp1_activation *new_act = new pp1_activation(t, decl);
          unsigned num_params = decl->formal_args.size();

          // (1a) restore parameter invocation closure
          if (num_params == 0 && decl->is_closure())
            {
              // NB: decl->parent_act is always safe since the
              // parameter decl (if any) comes from an activation
              // record which deeper in the stack than new_act.

              // decl is a macro parameter which must be evaluated in
              // the context of the original point of invocation:
              new_act->params = ((pp_macrodecl*)decl)->parent_act->params;
              goto expand;
            }

          // (1b) consume macro parameters (if any)
          if (num_params == 0)
            goto expand;

          // for simplicity, we do not allow macro constructs here
          // -- if we did, we'd have to recursively call scan_pp1()
          t = next_pp1 ();
          if (! (t && t->type == tok_operator && t->content == "("))
            {
              delete new_act;
              throw PARSE_ERROR (_NF
                                    ("expected '(' in invocation of macro '@%s'"
                                     " taking %d parameter",
                                     "expected '(' in invocation of macro '@%s'"
                                     " taking %d parameters",
                                     num_params, name.c_str(), num_params), t);
            }

          // XXX perhaps parse/count the full number of params,
          // so we can say "expected x, found y params" on error?
          for (unsigned i = 0; i < num_params; i++)
            {
              delete t;

              // create parameter closure
              string param_name = decl->formal_args[i];
              pp_macrodecl* p = (new_act->params[param_name]
                                 = new pp_macrodecl);
              p->tok = new token(*new_act->tok);
              p->parent_act = act;
              // NB: *new_act->tok points to invocation, act is NULL at top level

              t = slurp_pp1_param (p->body);

              // check correct usage of ',' or ')'
              if (t == 0) // hit unexpected EOF or end of macro
                {
                  // XXX could we pop the stack and continue parsing
                  // the invocation, allowing macros to construct new
                  // invocations in piecemeal fashion??
                  const token* orig_t = new token(*new_act->tok);
                  delete new_act;
                  throw PARSE_ERROR (_("could not find end of macro invocation"), orig_t);
                }
              if (t->type == tok_operator && t->content == ",")
                {
                  if (i + 1 == num_params)
                    {
                      delete new_act;
                      throw PARSE_ERROR (_F("too many parameters for macro '@%s' (expected %d)", name.c_str(), num_params), t);
                    }
                }
              else if (t->type == tok_operator && t->content == ")")
                {
                  if (i + 1 != num_params)
                    {
                      delete new_act;
                      throw PARSE_ERROR (_F("too few parameters for macro '@%s' (expected %d)", name.c_str(), num_params), t);
                    }
                }
              else
                {
                  // XXX this is, incidentally, impossible
                  delete new_act;
                  throw PARSE_ERROR(_("expected ',' or ')' after macro parameter"), t);
                }
            }

          delete t;

          // (2) set up macro expansion
        expand:
          pp1_state.push_back (new_act);

          // Now loop around to look for a real token.
          continue;
        }

      // Otherwise, we have an ordinary token.
      return t;
    }
}

// Consume a single macro invocation's parameters, heeding nesting
// brackets and stopping on an unbalanced ')' or an unbracketed ','
// (and returning the final separator token).
const token*
parser::slurp_pp1_param (vector<const token*>& param)
{
  const token* t = 0;
  unsigned nesting = 0;
  do
    {
      t = next_pp1 ();

      if (!t)
        break;
      // [ needed in case macro paramater is used as prefix for array-deref operation
      if (t->type == tok_operator && (t->content == "(" || t->content == "["))
        ++nesting;
      else if (nesting && t->type == tok_operator && (t->content == ")" || t->content == "]"))
        --nesting;
      else if (!nesting && t->type == tok_operator
               && (t->content == ")" || t->content == ","))
        break;
      param.push_back(t);
    }
  while (true);
  return t; // report ")" or "," or NULL
}


// Consume a macro declaration's body, heeding nested %( %) brackets.
const token*
parser::slurp_pp1_body (vector<const token*>& body)
{
  const token* t = 0;
  unsigned nesting = 0;
  do
    {
      t = next_pp1 ();

      if (!t)
        break;
      if (t->type == tok_operator && t->content == "%(")
        ++nesting;
      else if (nesting && t->type == tok_operator && t->content == "%)")
        --nesting;
      else if (!nesting && t->type == tok_operator && t->content == "%)")
        break;
      body.push_back(t);
    }
  while (true);
  return t; // report final "%)" or NULL
}

// Used for parsing .stpm files.
stapfile*
parser::parse_library_macros ()
{
  stapfile* f = new stapfile;
  input.set_current_file (f);

  try
    {
      const token* t = scan_pp ();

      // Currently we only take objection to macro invocations if they
      // produce a non-whitespace token after being expanded.

      // XXX should we prevent macro invocations even if they expand to empty??

      if (t != 0)
        throw PARSE_ERROR (_F("unexpected token in library macro file '%s'", input_name.c_str()), t);

      // We need to first check whether *any* of the macros are duplicates,
      // then commit to including the entire file in the global namespace
      // (or not). Yuck.
      for (map<string, macrodecl*>::iterator it = pp1_namespace.begin();
           it != pp1_namespace.end(); it++)
        {
          string name = it->first;

          if (session.library_macros.find(name) != session.library_macros.end())
            {
              parse_error er(ERR_SRC, _F("duplicate definition of library macro '@%s'", name.c_str()), it->second->tok);
              er.chain = new PARSE_ERROR (_F("macro '@%s' first defined here", name.c_str()), session.library_macros[name]->tok);
              print_error (er);

              delete er.chain;
              delete f;
              return 0;
            }
        }

    }
  catch (const parse_error& pe)
    {
      print_error (pe, errs_as_warnings);
      delete f;
      return 0;
    }

  // If no errors, include the entire file.  Note how this is outside
  // of the try-catch block -- no errors possible.
  for (map<string, macrodecl*>::iterator it = pp1_namespace.begin();
       it != pp1_namespace.end(); it++)
    {
      string name = it->first;
      
      session.library_macros[name] = it->second;
      session.library_macros[name]->context = ctx_library;
    }

  return f;
}

// Second pass - preprocessor conditional expansion.
//
// The basic form is %( CONDITION %? THEN-TOKENS %: ELSE-TOKENS %)
// where CONDITION is: kernel_v[r] COMPARISON-OP "version-string"
//                 or: arch COMPARISON-OP "arch-string"
//                 or: systemtap_v COMPARISON-OP "version-string"
//                 or: systemtap_privilege COMPARISON-OP "privilege-string"
//                 or: CONFIG_foo COMPARISON-OP "config-string"
//                 or: CONFIG_foo COMPARISON-OP number
//                 or: CONFIG_foo COMPARISON-OP CONFIG_bar
//                 or: "string1" COMPARISON-OP "string2"
//                 or: number1 COMPARISON-OP number2
// The %: ELSE-TOKENS part is optional.
//
// e.g. %( kernel_v > "2.5" %? "foo" %: "baz" %)
// e.g. %( arch != "i?86" %? "foo" %: "baz" %)
// e.g. %( CONFIG_foo %? "foo" %: "baz" %)
//
// Up to an entire %( ... %) expression is processed by a single call
// to this function.  Tokens included by any nested conditions are
// enqueued in a private vector.

bool eval_pp_conditional (systemtap_session& s,
                          const token* l, const token* op, const token* r)
{
  if (l->type == tok_identifier && (l->content == "kernel_v" ||
                                    l->content == "kernel_vr" || 
                                    l->content == "systemtap_v"))
    {
      if (! (r->type == tok_string))
        throw PARSE_ERROR (_("expected string literal"), r);

      string target_kernel_vr = s.kernel_release;
      string target_kernel_v = s.kernel_base_release;
      string target;

      if (l->content == "kernel_v") target = target_kernel_v;
      else if (l->content == "kernel_vr") target = target_kernel_vr;
      else if (l->content == "systemtap_v") target = s.compatible;
      else assert (0);

      string query = r->content;
      bool rhs_wildcard = (strpbrk (query.c_str(), "*?[") != 0);

      // collect acceptable strverscmp results.
      int rvc_ok1, rvc_ok2;
      bool wc_ok = false;
      if (op->type == tok_operator && op->content == "<=")
        { rvc_ok1 = -1; rvc_ok2 = 0; }
      else if (op->type == tok_operator && op->content == ">=")
        { rvc_ok1 = 1; rvc_ok2 = 0; }
      else if (op->type == tok_operator && op->content == "<")
        { rvc_ok1 = -1; rvc_ok2 = -1; }
      else if (op->type == tok_operator && op->content == ">")
        { rvc_ok1 = 1; rvc_ok2 = 1; }
      else if (op->type == tok_operator && op->content == "==")
        { rvc_ok1 = 0; rvc_ok2 = 0; wc_ok = true; }
      else if (op->type == tok_operator && op->content == "!=")
        { rvc_ok1 = -1; rvc_ok2 = 1; wc_ok = true; }
      else
        throw PARSE_ERROR (_("expected comparison operator"), op);

      if ((!wc_ok) && rhs_wildcard)
        throw PARSE_ERROR (_("wildcard not allowed with order comparison operators"), op);

      if (rhs_wildcard)
        {
          int rvc_result = fnmatch (query.c_str(), target.c_str(),
                                    FNM_NOESCAPE); // spooky
          bool badness = (rvc_result == 0) ^ (op->content == "==");
          return !badness;
        }
      else
        {
          int rvc_result = strverscmp (target.c_str(), query.c_str());
          // normalize rvc_result
          if (rvc_result < 0) rvc_result = -1;
          if (rvc_result > 0) rvc_result = 1;
          return (rvc_result == rvc_ok1 || rvc_result == rvc_ok2);
        }
    }
  else if (l->type == tok_identifier && l->content == "systemtap_privilege")
    {
      string target_privilege =
	pr_contains(s.privilege, pr_stapdev) ? "stapdev"
	: pr_contains(s.privilege, pr_stapsys) ? "stapsys"
	: pr_contains(s.privilege, pr_stapusr) ? "stapusr"
	: "none"; /* should be impossible -- s.privilege always one of above */
      assert(target_privilege != "none");

      if (! (r->type == tok_string))
        throw PARSE_ERROR (_("expected string literal"), r);
      string query_privilege = r->content;

      bool nomatch = (target_privilege != query_privilege);

      bool result;
      if (op->type == tok_operator && op->content == "==")
        result = !nomatch;
      else if (op->type == tok_operator && op->content == "!=")
        result = nomatch;
      else
        throw PARSE_ERROR (_("expected '==' or '!='"), op);
      /* XXX perhaps allow <= >= and similar comparisons */

      return result;
    }
  else if (l->type == tok_identifier && l->content == "guru_mode")
    {
      if (! (r->type == tok_number))
        throw PARSE_ERROR (_("expected number"), r);
      int64_t lhs = (int64_t) s.guru_mode;
      int64_t rhs = lex_cast<int64_t>(r->content);
      if (!((rhs == 0)||(rhs == 1)))
        throw PARSE_ERROR (_("expected 0 or 1"), op);
      if (!((op->type == tok_operator && op->content == "==") ||
	    (op->type == tok_operator && op->content == "!=")))
        throw PARSE_ERROR (_("expected '==' or '!='"), op);

      return eval_comparison (lhs, op, rhs);
    }
  else if (l->type == tok_identifier && l->content == "arch")
    {
      string target_architecture = s.architecture;
      if (! (r->type == tok_string))
        throw PARSE_ERROR (_("expected string literal"), r);
      string query_architecture = r->content;

      int nomatch = fnmatch (query_architecture.c_str(),
                             target_architecture.c_str(),
                             FNM_NOESCAPE); // still spooky

      bool result;
      if (op->type == tok_operator && op->content == "==")
        result = !nomatch;
      else if (op->type == tok_operator && op->content == "!=")
        result = nomatch;
      else
        throw PARSE_ERROR (_("expected '==' or '!='"), op);

      return result;
    }
  else if (l->type == tok_identifier && l->content == "runtime")
    {
      if (! (r->type == tok_string))
        throw PARSE_ERROR (_("expected string literal"), r);

      string query_runtime = r->content;
      string target_runtime;

      if (s.runtime_mode == systemtap_session::dyninst_runtime)
        target_runtime = "dyninst";
      else if (s.runtime_mode == systemtap_session::bpf_runtime)
        target_runtime = "bpf";
      else
        target_runtime = "kernel";

      int nomatch = fnmatch (query_runtime.c_str(),
                             target_runtime.c_str(),
                             FNM_NOESCAPE); // still spooky

      bool result;
      if (op->type == tok_operator && op->content == "==")
        result = !nomatch;
      else if (op->type == tok_operator && op->content == "!=")
        result = nomatch;
      else
        throw PARSE_ERROR (_("expected '==' or '!='"), op);

      return result;
    }
  else if (l->type == tok_identifier && l->content.starts_with("CONFIG_"))
    {
      if (r->type == tok_string)
	{
	  string lhs = s.kernel_config[l->content]; // may be empty
	  string rhs = r->content;

	  int nomatch = fnmatch (rhs.c_str(), lhs.c_str(), FNM_NOESCAPE); // still spooky

	  bool result;
	  if (op->type == tok_operator && op->content == "==")
	    result = !nomatch;
	  else if (op->type == tok_operator && op->content == "!=")
	    result = nomatch;
	  else
	    throw PARSE_ERROR (_("expected '==' or '!='"), op);

	  return result;
	}
      else if (r->type == tok_number)
	{
          const string& lhs_string = s.kernel_config[l->content];
          const char* startp = lhs_string.c_str ();
          char* endp = (char*) startp;
          errno = 0;
          int64_t lhs = (int64_t) strtoll (startp, & endp, 0);
          if (errno == ERANGE || errno == EINVAL || *endp != '\0')
	    throw PARSE_ERROR ("Config option value not a number", l);

	  int64_t rhs = lex_cast<int64_t>(r->content);
	  return eval_comparison (lhs, op, rhs);
	}
      else if (r->type == tok_identifier
	       && r->content.starts_with( "CONFIG_"))
	{
	  // First try to convert both to numbers,
	  // otherwise threat both as strings.
          const string& lhs_string = s.kernel_config[l->content];
          const string& rhs_string = s.kernel_config[r->content];
          const char* startp = lhs_string.c_str ();
          char* endp = (char*) startp;
          errno = 0;
          int64_t val = (int64_t) strtoll (startp, & endp, 0);
          if (errno != ERANGE && errno != EINVAL && *endp == '\0')
	    {
	      int64_t lhs = val;
              startp = rhs_string.c_str ();
	      endp = (char*) startp;
	      errno = 0;
	      int64_t rhs = (int64_t) strtoll (startp, & endp, 0);
	      if (errno != ERANGE && errno != EINVAL && *endp == '\0')
		return eval_comparison (lhs, op, rhs);
	    }

	  return eval_comparison (lhs_string, op, rhs_string);
	}
      else
	throw PARSE_ERROR (_("expected string, number literal or other CONFIG_... as right side operand"), r);
    }
  else if (l->type == tok_string && r->type == tok_string)
    {
      string lhs = l->content;
      string rhs = r->content;
      return eval_comparison (lhs, op, rhs);
      // NB: no wildcarding option here
    }
  else if (l->type == tok_number && r->type == tok_number)
    {
      int64_t lhs = lex_cast<int64_t>(l->content);
      int64_t rhs = lex_cast<int64_t>(r->content);
      return eval_comparison (lhs, op, rhs);
      // NB: no wildcarding option here
    }
  else if (l->type == tok_string && r->type == tok_number
	    && op->type == tok_operator)
    throw PARSE_ERROR (_("expected string literal as right value"), r);
  else if (l->type == tok_number && r->type == tok_string
	    && op->type == tok_operator)
    throw PARSE_ERROR (_("expected number literal as right value"), r);

  else
    throw PARSE_ERROR (_("expected 'arch', 'kernel_v', 'kernel_vr', 'systemtap_v',\n"
			 "             'runtime', 'systemtap_privilege', 'CONFIG_...', or\n"
			 "             comparison between strings or integers"), l);
}


// Only tokens corresponding to the TRUE statement must be expanded
const token*
parser::scan_pp ()
{
  while (true)
    {
      pp_state_t pp = PP_NONE;
      if (!pp_state.empty())
        pp = pp_state.back().second;

      const token* t = 0;
      if (pp == PP_SKIP_THEN || pp == PP_SKIP_ELSE)
        t = skip_pp ();
      else
        t = scan_pp1 ();

      if (t == 0) // EOF
        {
          if (pp != PP_NONE)
            {
              t = pp_state.back().first;
              pp_state.pop_back(); // so skip_some doesn't keep trying to close this
              //TRANSLATORS: 'conditional' meaning 'conditional preprocessing'
              throw PARSE_ERROR (_("incomplete conditional at end of file"), t);
            }
          return t;
        }

      // misplaced preprocessor "then"
      if (t->type == tok_operator && t->content == "%?")
        throw PARSE_ERROR (_("incomplete conditional - missing '%('"), t);

      // preprocessor "else"
      if (t->type == tok_operator && t->content == "%:")
        {
          if (pp == PP_NONE)
            throw PARSE_ERROR (_("incomplete conditional - missing '%('"), t);
          if (pp == PP_KEEP_ELSE || pp == PP_SKIP_ELSE)
            throw PARSE_ERROR (_("invalid conditional - duplicate '%:'"), t);
          // XXX: here and elsewhere, error cascades might be avoided
          // by dropping tokens until we reach the closing %)

          pp_state.back().second = (pp == PP_KEEP_THEN) ?
                                   PP_SKIP_ELSE : PP_KEEP_ELSE;
          delete t;
          continue;
        }

      // preprocessor close
      if (t->type == tok_operator && t->content == "%)")
        {
          if (pp == PP_NONE)
            throw PARSE_ERROR (_("incomplete conditional - missing '%('"), t);
          delete pp_state.back().first;
          delete t; //this is the closing bracket
          pp_state.pop_back();
          continue;
        }

      if (! (t->type == tok_operator && t->content == "%(")) // ordinary token
        return t;

      // We have a %( - it's time to throw a preprocessing party!

      bool result = false;
      bool and_result = true;
      const token *n = NULL;
      do {
        const token *l, *op, *r;
        l = scan_pp1 ();
        op = scan_pp1 ();
        r = scan_pp1 ();
        if (l == 0 || op == 0 || r == 0)
          throw PARSE_ERROR (_("incomplete condition after '%('"), t);
        // NB: consider generalizing to consume all tokens until %?, and
        // passing that as a vector to an evaluator.

        // Do not evaluate the condition if we haven't expanded everything.
        // This may occur when having several recursive conditionals.
        and_result &= eval_pp_conditional (session, l, op, r);
        if(l->content=="systemtap_v")
          systemtap_v_seen=r;

        else
          delete r;

        delete l;
        delete op;
        delete n;

        n = scan_pp1 ();
        if (n && n->type == tok_operator && n->content == "&&")
          continue;
        result |= and_result;
        and_result = true;
        if (! (n && n->type == tok_operator && n->content == "||"))
          break;
      } while (true);

      /*
      clog << "PP eval (" << *t << ") == " << result << endl;
      */

      const token *m = n;
      if (! (m && m->type == tok_operator && m->content == "%?"))
        throw PARSE_ERROR (_("expected '%?' marker for conditional"), t);
      delete m; // "%?"

      pp = result ? PP_KEEP_THEN : PP_SKIP_THEN;
      pp_state.push_back (make_pair (t, pp));

      // Now loop around to look for a real token.
    }
}


// Skip over tokens and any errors, heeding
// only nested preprocessor starts and ends.
const token*
parser::skip_pp ()
{
  const token* t = 0;
  unsigned nesting = 0;
  do
    {
      try
        {
          t = scan_pp1 (true);
        }
      catch (const parse_error &e)
        {
          continue;
        }
      if (!t)
        break;
      if (t->type == tok_operator && t->content == "%(")
        ++nesting;
      else if (nesting && t->type == tok_operator && t->content == "%)")
        --nesting;
      else if (!nesting && t->type == tok_operator &&
               (t->content == "%:" || t->content == "%?" || t->content == "%)"))
        break;
      delete t;
    }
  while (true);
  return t;
}


const token*
parser::next ()
{
  if (! next_t)
    next_t = scan_pp ();
  if (! next_t)
    throw PARSE_ERROR (_("unexpected end-of-file"));

  last_t = next_t;
  // advance by zeroing next_t
  next_t = 0;
  return last_t;
}


const token*
parser::peek ()
{
  if (! next_t)
    next_t = scan_pp ();

  // don't advance by zeroing next_t
  last_t = next_t;
  return next_t;
}


void
parser::swallow ()
{
  // can only swallow something last peeked or nexted token.
  assert (last_t != 0);
  delete last_t;
  // advance by zeroing next_t
  last_t = next_t = 0;
}


static inline bool
tok_is(token const * t, token_type tt, string const & expected)
{
  return t && t->type == tt && t->content == expected;
}


void
parser::expect_known (token_type tt, string const & expected)
{
  const token *t = next();
  if (! (t && t->type == tt && t->content == expected))
    throw PARSE_ERROR (_F("expected '%s'", expected.c_str()));
  swallow (); // We are done with it, content was copied.
}


void
parser::expect_unknown (token_type tt, interned_string & target)
{
  const token *t = next();
  if (!(t && t->type == tt))
    throw PARSE_ERROR (_("expected ") + tt2str(tt));
  target = t->content;
  swallow (); // We are done with it, content was copied.
}


void
parser::expect_unknown2 (token_type tt1, token_type tt2, interned_string & target)
{
  const token *t = next();
  if (!(t && (t->type == tt1 || t->type == tt2)))
    throw PARSE_ERROR (_F("expected %s or %s", tt2str(tt1).c_str(), tt2str(tt2).c_str()));
  target = t->content;
  swallow (); // We are done with it, content was copied.
}


void
parser::expect_op (string const & expected)
{
  expect_known (tok_operator, expected);
}

interned_string
parser::expect_op_any (initializer_list<const char*> expected)
{
  const token *t = next();
  if (t && t->type == tok_operator)
    for (auto it = expected.begin(); it != expected.end(); ++it)
      if (t->content == *it)
        {
          interned_string found = t->content;
          swallow (); // We are done with it, content was copied.
          return found;
        }

  string msg;
  for (auto it = expected.begin(); it != expected.end(); ++it)
    {
      if (it != expected.begin())
        msg.append(" ");
      msg.append(*it);
    }
  throw PARSE_ERROR (_F("expected one of '%s'", msg.c_str()));
}

void
parser::expect_kw (string const & expected)
{
  expect_known (tok_keyword, expected);
}

const token*
parser::expect_kw_token (string const & expected)
{
  const token *t = next();
  if (! (t && t->type == tok_keyword && t->content == expected))
    throw PARSE_ERROR (_F("expected '%s'", expected.c_str()));
  return t;
}

void
parser::expect_number (int64_t & value)
{
  bool neg = false;
  const token *t = next();
  if (t->type == tok_operator && t->content == "-")
    {
      neg = true;
      swallow ();
      t = next ();
    }
  if (!(t && t->type == tok_number))
    throw PARSE_ERROR (_("expected number"));

  const string& s = t->content;
  const char* startp = s.c_str ();
  char* endp = (char*) startp;

  // NB: we allow controlled overflow from LLONG_MIN .. ULLONG_MAX
  // Actually, this allows all the way from -ULLONG_MAX to ULLONG_MAX,
  // since the lexer only gives us positive digit strings, but we'll
  // limit it to LLONG_MIN when a '-' operator is fed into the literal.
  errno = 0;
  value = (int64_t) strtoull (startp, & endp, 0);
  if (errno == ERANGE || errno == EINVAL || *endp != '\0'
      || (neg && (unsigned long long) value > 9223372036854775808ULL)
      || (unsigned long long) value > 18446744073709551615ULL
      || value < -9223372036854775807LL-1)
    throw PARSE_ERROR (_("number invalid or out of range"));

  if (neg)
    value = -value;

  swallow (); // We are done with it, content was parsed and copied into value.
}


const token*
parser::expect_ident_or_atword (interned_string & target)
{
  const token *t = next();

  // accept identifiers and operators beginning in '@':
  if (!t || (t->type != tok_identifier
             && (t->type != tok_operator || t->content[0] != '@')))
    // XXX currently this is only called from parse_hist_op_or_bare_name(),
    // so the message is accurate, but keep an eye out in the future:
    throw PARSE_ERROR (_F("expected %s or statistical operation", tt2str(tok_identifier).c_str()));

  target = t->content;
  return t;
}


void
parser::expect_ident_or_keyword (interned_string & target)
{
  expect_unknown2 (tok_identifier, tok_keyword, target);
}


bool
parser::peek_op (string const & op)
{
  return tok_is (peek(), tok_operator, op);
}


bool
parser::peek_kw (string const & kw)
{
  return tok_is (peek(), tok_identifier, kw);
}



lexer::lexer (istream& input, const string& in, systemtap_session& s, bool cc):
  ate_comment(false), ate_whitespace(false), saw_tokens(false), check_compatible(cc),
  input_name (in), input_pointer (0), input_end (0), cursor_suspend_count(0),
  cursor_suspend_line (1), cursor_suspend_column (1), cursor_line (1),
  cursor_column (1), session(s), current_file (0), current_token_chain (0)
{
  getline(input, input_contents, '\0');

  input_pointer = input_contents.data();
  input_end = input_contents.data() + input_contents.size();

  if (keywords.empty())
    {
      // NB: adding new keywords is highly disruptive to the language,
      // in particular to existing scripts that could be suddenly
      // broken.  If done at all, it has to be s.compatible-sensitive,
      // and broadly advertised.
      keywords.insert("probe");
      keywords.insert("global");
      if (has_version("3.0"))
        keywords.insert("private");
      keywords.insert("function");
      keywords.insert("if");
      keywords.insert("else");
      keywords.insert("for");
      keywords.insert("foreach");
      keywords.insert("in");
      keywords.insert("limit");
      keywords.insert("return");
      keywords.insert("delete");
      keywords.insert("while");
      keywords.insert("break");
      keywords.insert("continue");
      keywords.insert("next");
      keywords.insert("string");
      keywords.insert("long");
      keywords.insert("try");
      keywords.insert("catch");
    }

  if (atwords.empty())
    {
      // NB: adding new @words is mildly disruptive to existing
      // scripts that define macros with the same name, but not
      // really. The user will merely receive a warning that they are
      // redefining an existing operator.

      // These are inserted without the actual '@', so we can directly check
      // proposed macro names without building a string with that prefix.
      atwords.insert("cast");
      atwords.insert("defined");
      atwords.insert("entry");
      atwords.insert("perf");
      atwords.insert("var");
      atwords.insert("avg");
      atwords.insert("count");
      atwords.insert("sum");
      atwords.insert("min");
      atwords.insert("max");
      atwords.insert("hist_linear");
      atwords.insert("hist_log");
      if (has_version("3.1"))
        {
          atwords.insert("const");
          atwords.insert("variance");
        }
    }
}

unordered_set<string> lexer::atwords;

void
lexer::set_current_file (stapfile* f)
{
  current_file = f;
  if (f)
    {
      f->file_contents = input_contents;
      f->name = input_name;
    }
}

void
lexer::set_current_token_chain (const token* tok)
{
  current_token_chain = tok;
}

int
lexer::input_peek (unsigned n)
{
  if (input_pointer + n >= input_end)
    return -1; // EOF
  return (unsigned char)*(input_pointer + n);
}


bool
lexer::has_version (const char* v) const
{
  return check_compatible
    ? strverscmp(session.compatible.c_str(), v) >= 0
    : true;
}

int
lexer::input_get ()
{
  int c = input_peek();
  if (c < 0) return c; // EOF

  ++input_pointer;

  if (cursor_suspend_count)
    {
      // Track effect of input_put: preserve previous cursor/line_column
      // until all of its characters are consumed.
      if (--cursor_suspend_count == 0)
        {
          cursor_line = cursor_suspend_line;
          cursor_column = cursor_suspend_column;
        }
    }
  else
    {
      // update source cursor
      if (c == '\n')
        {
          cursor_line ++;
          cursor_column = 1;
        }
      else
        cursor_column ++;
    }

  // clog << "[" << (char)c << "]";
  return c;
}


void
lexer::input_put (const string& chars, const token* t)
{
  size_t pos = input_pointer - input_contents.data();
  // clog << "[put:" << chars << " @" << pos << "]";
  input_contents.insert (pos, chars);
  cursor_suspend_count += chars.size();
  cursor_suspend_line = cursor_line;
  cursor_suspend_column = cursor_column;
  cursor_line = t->location.line;
  cursor_column = t->location.column;
  input_pointer = input_contents.data() + pos;
  input_end = input_contents.data() + input_contents.size();
}


token*
lexer::scan ()
{
  ate_comment = false; // reset for each new token
  ate_whitespace = false; // reset for each new token

  // XXX be very sure to restore old_saw_tokens if we return without a token:
  bool old_saw_tokens = saw_tokens;
  saw_tokens = true;

  token* n = new token;
  string token_str; // accumulate here instead of by incremental interning
  n->location.file = current_file;
  n->chain = current_token_chain;

skip:
  bool suspended = (cursor_suspend_count > 0);
  n->location.line = cursor_line;
  n->location.column = cursor_column;

  int c = input_get();
  // clog << "{" << (char)c << (char)c2 << "}";
  if (c < 0)
    {
      delete n;
      saw_tokens = old_saw_tokens;
      return 0;
    }

  if (isspace (c))
    {
      ate_whitespace = true;
      goto skip;
    }

  int c2 = input_peek ();

  // Paste command line arguments as character streams into
  // the beginning of a token.  $1..$999 go through as raw
  // characters; @1..@999 are quoted/escaped as strings.
  // $# and @# expand to the number of arguments, similarly
  // raw or quoted.
  if ((c == '$' || c == '@') && (c2 == '#'))
    {
      token_str.push_back (c);
      token_str.push_back (c2);
      input_get(); // swallow '#'

      if (suspended)
        {
          n->make_junk(tok_junk_nested_arg);
          return n;
        }
      size_t num_args = session.args.size ();
      input_put ((c == '$') ? lex_cast (num_args) : lex_cast_qstring (num_args), n);
      token_str.clear();
      goto skip;
    }
  else if ((c == '$' || c == '@') && (isdigit (c2)))
    {
      unsigned idx = 0;
      token_str.push_back (c);
      do
        {
          input_get ();
          token_str.push_back (c2);
          idx = (idx * 10) + (c2 - '0');
          c2 = input_peek ();
        } while (c2 > 0 &&
                 isdigit (c2) &&
                 idx <= session.args.size()); // prevent overflow
      if (suspended) 
        {
          n->make_junk(tok_junk_nested_arg);
          return n;
        }
      if (idx == 0 ||
          idx-1 >= session.args.size())
        {
          n->make_junk(tok_junk_invalid_arg);
          return n;
        }
      session.used_args[idx-1] = true;
      const string& arg = session.args[idx-1];
      input_put ((c == '$') ? arg : lex_cast_qstring (arg), n);
      token_str.clear();
      goto skip;
    }

  else if (isalpha (c) || c == '$' || c == '@' || c == '_')
    {
      token_str = (char) c;
      while (isalnum (c2) || c2 == '_' || c2 == '$')
	{
          input_get ();
          token_str.push_back (c2);
          c2 = input_peek ();
        }
      n->content = token_str;

      if (n->content[0] == '@')
        // makes it easier to detect illegal use of @words:
        n->type = tok_operator;
      else if (keywords.count(n->content))
        n->type = tok_keyword;
      else
        n->type = tok_identifier;

      return n;
    }

  else if (isdigit (c)) // positive literal
    {
      n->type = tok_number;
      token_str = (char) c;

      while (isalnum (c2))
	{
          // NB: isalnum is very permissive.  We rely on strtol, called in
          // parser::parse_literal below, to confirm that the number string
          // is correctly formatted and in range.

          input_get ();
          token_str.push_back (c2);
          c2 = input_peek ();
	}

      n->content = token_str;
      return n;
    }

  else if (c == '\"')
    {
      n->type = tok_string;
      while (1)
	{
	  c = input_get ();

	  if (c < 0 || c == '\n')
	    {
              n->make_junk(tok_junk_unclosed_quote);
              return n;
	    }
	  if (c == '\"') // closing double-quotes
	    break;
	  else if (c == '\\') // see also input_put
	    {
	      c = input_get();
	      switch (c)
		{
                case 'x':
                  if (!has_version("2.3"))
                    goto the_default;
                  /* FALLTHROUGH */
		case 'a':
		case 'b':
		case 't':
		case 'n':
		case 'v':
		case 'f':
		case 'r':
		case '0' ... '7': // NB: need only match the first digit
		case '\\':
		  // Pass these escapes through to the string value
		  // being parsed; it will be emitted into a C literal.
                  // XXX: PR13371: perhaps we should evaluate them here
                  // (and re-quote them during translate.cxx emission).
                  token_str.push_back ('\\');

                  // fall through
		default: the_default:
                  token_str.push_back (c);
                  break;
		}
	    }
	  else
            token_str.push_back (c);
	}
      n->content = token_str;
      return n;
    }

  else if (ispunct (c))
    {
      int c3 = input_peek (1);

      // NB: if we were to recognize negative numeric literals here,
      // we'd introduce another grammar ambiguity:
      // 1-1 would be parsed as tok_number(1) and tok_number(-1)
      // instead of tok_number(1) tok_operator('-') tok_number(1)

      if (c == '#') // shell comment
        {
          unsigned this_line = cursor_line;
          do { c = input_get (); }
          while (c >= 0 && cursor_line == this_line);
          ate_comment = true;
          ate_whitespace = true;
          goto skip;
        }
      else if ((c == '/' && c2 == '/')) // C++ comment
        {
          unsigned this_line = cursor_line;
          do { c = input_get (); }
          while (c >= 0 && cursor_line == this_line);
          ate_comment = true;
          ate_whitespace = true;
          goto skip;
        }
      else if (c == '/' && c2 == '*') // C comment
	{
          (void) input_get (); // swallow '*' already in c2
          c = input_get ();
          c2 = input_get ();
          while (c2 >= 0)
            {
              if (c == '*' && c2 == '/')
                break;
              c = c2;
              c2 = input_get ();
            }
          ate_comment = true;
          ate_whitespace = true;
          goto skip;
	}
      else if (c == '%' && c2 == '{') // embedded code
        {
          n->type = tok_embedded;
          (void) input_get (); // swallow '{' already in c2
          c = input_get ();
          c2 = input_get ();
          while (c2 >= 0)
            {
              if (c == '%' && c2 == '}')
                {
                  n->content = token_str;
                  return n;
                }
              if (c == '}' && c2 == '%') // possible typo
                session.print_warning (_("possible erroneous closing '}%', use '%}'?"), n);
              token_str.push_back (c);
              c = c2;
              c2 = input_get();
            }

            n->make_junk(tok_junk_unclosed_embedded);
            return n;
        }

      // We're committed to recognizing at least the first character
      // as an operator.
      n->type = tok_operator;
      token_str = (char) c;

      // match all valid operators, in decreasing size order
      if ((c == '<' && c2 == '<' && c3 == '<') ||
          (c == '>' && c2 == '>' && c3 == '>') ||
          (c == '<' && c2 == '<' && c3 == '=') ||
          (c == '>' && c2 == '>' && c3 == '='))
        {
          token_str.push_back (c2);
          token_str.push_back (c3);
          input_get (); // c2
          input_get (); // c3
        }
      else if ((c == '=' && c2 == '=') ||
               (c == '!' && c2 == '=') ||
               (c == '<' && c2 == '=') ||
               (c == '>' && c2 == '=') ||
               (c == '=' && c2 == '~') ||
               (c == '!' && c2 == '~') ||
               (c == '+' && c2 == '=') ||
               (c == '-' && c2 == '=') ||
               (c == '*' && c2 == '=') ||
               (c == '/' && c2 == '=') ||
               (c == '%' && c2 == '=') ||
               (c == '&' && c2 == '=') ||
               (c == '^' && c2 == '=') ||
               (c == '|' && c2 == '=') ||
               (c == '.' && c2 == '=') ||
               (c == '&' && c2 == '&') ||
               (c == '|' && c2 == '|') ||
               (c == '+' && c2 == '+') ||
               (c == '-' && c2 == '-') ||
               (c == '-' && c2 == '>') ||
               (c == '<' && c2 == '<') ||
               (c == '>' && c2 == '>') ||
               // preprocessor tokens
               (c == '%' && c2 == '(') ||
               (c == '%' && c2 == '?') ||
               (c == '%' && c2 == ':') ||
               (c == '%' && c2 == ')'))
        {
          token_str.push_back (c2);
          input_get (); // swallow other character
        }

      n->content = token_str;
      return n;
    }

  else
    {
      n->type = tok_junk;
      ostringstream s;
      s << "\\x" << hex << setw(2) << setfill('0') << c;
      n->content = s.str();
      // signal parser to emit "expected X, found junk" type error
      n->make_junk(tok_junk_unknown);
      return n;
    }
}

// ------------------------------------------------------------------------

void
token::make_junk (token_junk_type junk)
{
  type = tok_junk;
  junk_type = junk;
}

// ------------------------------------------------------------------------

string
token::junk_message(systemtap_session& session) const
{
  switch (junk_type)
    {
    case tok_junk_nested_arg:
      return _("invalid nested substitution of command line arguments");

    case tok_junk_invalid_arg:
      return _F("command line argument out of range [1-%lu]",
                (unsigned long) session.args.size());

    case tok_junk_unclosed_quote:
      return _("Could not find matching closing quote");

    case tok_junk_unclosed_embedded:
      return _("Could not find matching '%}' to close embedded function block");

    default:
      return _("unknown junk token");
    }
}

// ------------------------------------------------------------------------

stapfile*
parser::parse ()
{
  stapfile* f = new stapfile;
  input.set_current_file (f);

  bool empty = true;

  while (1)
    {
      try
	{
          systemtap_v_seen = 0;
	  const token* t = peek ();
	  if (! t) // nice clean EOF, modulo any preprocessing that occurred
	    break;

          empty = false;
	  if (t->type == tok_keyword && t->content == "probe")
	    {
	      context = con_probe;
	      parse_probe (f->probes, f->aliases);
	    }
	  else if (t->type == tok_keyword && t->content == "private")
	    {
	      context = con_unknown;
	      parse_private (f->globals, f->probes, f->name, f->functions);
	    }
	  else if (t->type == tok_keyword && t->content == "global")
	    {
	      context = con_global;
	      parse_global (f->globals, f->probes, f->name);
	    }
	  else if (t->type == tok_keyword && t->content == "function")
	    {
	      context = con_function;
	      parse_functiondecl (f->functions, f->name);
	    }
          else if (t->type == tok_embedded)
	    {
	      context = con_embedded;
	      f->embeds.push_back (parse_embeddedcode ());
	    }
	  else
	    {
	      context = con_unknown;
	      throw PARSE_ERROR (_("expected 'probe', 'global', 'private', 'function', or '%{'"));
	    }
	}
      catch (parse_error& pe)
	{
	  print_error (pe, errs_as_warnings);

          // XXX: do we want tok_junk to be able to force skip_some behaviour?
          if (pe.skip_some) // for recovery
            // Quietly swallow all tokens until the next keyword we can start parsing from.
            while (1)
              try
                {
                  {
                    const token* t = peek ();
                    if (! t)
                      break;
                    if (t->type == tok_keyword && t->content == "probe") break;
                    else if (t->type == tok_keyword && t->content == "private") break;
                    else if (t->type == tok_keyword && t->content == "global") break;
                    else if (t->type == tok_keyword && t->content == "function") break;
                    else if (t->type == tok_embedded) break;
                    swallow (); // swallow it
                  }
                }
              catch (parse_error& pe2)
                {
                  // parse error during recovery ... ugh
                  print_error (pe2);
                }
        }
    }

  if (empty && user_file)
    {
      // vary message depending on whether file was *actually* empty:
      cerr << (input.saw_tokens
               ? _F("Input file '%s' is empty after preprocessing.", input_name.c_str())
               : _F("Input file '%s' is empty.", input_name.c_str()))
           << endl;
      delete f;
      f = 0;
    }
  else if (num_errors > 0)
    {
      cerr << _NF("%d parse error.", "%d parse errors.", num_errors, num_errors) << endl;
      delete f;
      f = 0;
    }

  input.set_current_file(0);
  return f;
}


probe*
parser::parse_synthetic_probe (const token* chain)
{
  probe* p = NULL;
  stapfile* f = new stapfile;
  f->synthetic = true;
  input.set_current_file (f);
  input.set_current_token_chain (chain);

  try
    {
      context = con_probe;
      parse_probe (f->probes, f->aliases);

      if (f->probes.size() != 1 || !f->aliases.empty())
        throw PARSE_ERROR (_("expected a single synthetic probe"));
      p = f->probes[0];
    }
  catch (parse_error& pe)
    {
      print_error (pe, errs_as_warnings);
    }

  // TODO check for unparsed tokens?

  input.set_current_file(0);
  input.set_current_token_chain(0);
  p->synthetic = true;
  return p;
}


void
parser::parse_probe (vector<probe *> & probe_ret,
		     vector<probe_alias *> & alias_ret)
{
  const token* t0 = next ();
  if (! (t0->type == tok_keyword && t0->content == "probe"))
    throw PARSE_ERROR (_("expected 'probe'"));

  vector<probe_point *> aliases;
  vector<probe_point *> locations;

  int epilogue_alias = 0;

  while (1)
    {
      vector<probe_point*> pps = parse_probe_points();

      const token* t = peek ();
      if (pps.size() == 1 && t
          && t->type == tok_operator && t->content == "=")
        {
          if (pps[0]->optional || pps[0]->sufficient)
            throw PARSE_ERROR (_("probe point alias name cannot be optional nor sufficient"), pps[0]->components.front()->tok);
          aliases.push_back(pps[0]);
          swallow ();
          continue;
        }
      else if (pps.size() == 1 && t
          && t->type == tok_operator && t->content == "+=")
        {
          if (pps[0]->optional || pps[0]->sufficient)
            throw PARSE_ERROR (_("probe point alias name cannot be optional nor sufficient"), pps[0]->components.front()->tok);
          aliases.push_back(pps[0]);
          epilogue_alias = 1;
          swallow ();
          continue;
        }
      else if (t && t->type == tok_operator && t->content == "{")
        {
          locations.insert(locations.end(), pps.begin(), pps.end());
          break;
        }
      else
	throw PARSE_ERROR (_("expected probe point specifier"));
    }

  if (aliases.empty())
    {
      probe* p = new probe;
      p->tok = t0;
      p->locations = locations;
      p->body = parse_stmt_block ();
      p->privileged = privileged;
      p->systemtap_v_conditional = systemtap_v_seen;
      probe_ret.push_back (p);
    }
  else
    {
      probe_alias* p = new probe_alias (aliases);
      if(epilogue_alias)
	p->epilogue_style = true;
      else
	p->epilogue_style = false;
      p->tok = t0;
      p->locations = locations;
      p->body = parse_stmt_block ();
      p->privileged = privileged;
      p->systemtap_v_conditional = systemtap_v_seen;
      alias_ret.push_back (p);
    }
}


embeddedcode*
parser::parse_embeddedcode ()
{
  embeddedcode* e = new embeddedcode;
  const token* t = next ();
  if (t->type != tok_embedded)
    throw PARSE_ERROR (_("expected '%{'"));

  if (! privileged)
    throw PARSE_ERROR (_("embedded code in unprivileged script; need stap -g"),
                       false /* don't skip tokens for parse resumption */);

  e->tok = t;
  e->code = t->content;
  return e;
}


block*
parser::parse_stmt_block ()
{
  block* pb = new block;

  const token* t = next ();
  if (! (t->type == tok_operator && t->content == "{"))
    throw PARSE_ERROR (_("expected '{'"));

  pb->tok = t;

  while (1)
    {
      t = peek ();
      if (t && t->type == tok_operator && t->content == "}")
        {
          swallow ();
          break;
        }
      pb->statements.push_back (parse_statement ());
    }

  return pb;
}


try_block*
parser::parse_try_block ()
{
  try_block* pb = new try_block;

  pb->tok = expect_kw_token ("try");
  pb->try_block = parse_stmt_block();
  expect_kw ("catch");

  const token* t = peek ();
  if (t != NULL && t->type == tok_operator && t->content == "(")
    {
      swallow (); // swallow the '('

      t = next();
      if (! (t->type == tok_identifier))
        throw PARSE_ERROR (_("expected identifier"));
      symbol* sym = new symbol;
      sym->tok = t;
      sym->name = t->content;
      pb->catch_error_var = sym;

      expect_op (")");
    }
  else
    pb->catch_error_var = 0;

  pb->catch_block = parse_stmt_block();

  return pb;
}



statement*
parser::parse_statement ()
{
  statement *ret;
  const token* t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    return new null_statement (next ());
  else if (t && t->type == tok_operator && t->content == "{")
    return parse_stmt_block (); // Don't squash semicolons.
  else if (t && t->type == tok_keyword && t->content == "try")
    return parse_try_block (); // Don't squash semicolons.
  else if (t && t->type == tok_keyword && t->content == "if")
    return parse_if_statement (); // Don't squash semicolons.
  else if (t && t->type == tok_keyword && t->content == "for")
    return parse_for_loop (); // Don't squash semicolons.
  else if (t && t->type == tok_keyword && t->content == "foreach")
    return parse_foreach_loop (); // Don't squash semicolons.
  else if (t && t->type == tok_keyword && t->content == "while")
    return parse_while_loop (); // Don't squash semicolons.
  else if (t && t->type == tok_keyword && t->content == "return")
    ret = parse_return_statement ();
  else if (t && t->type == tok_keyword && t->content == "delete")
    ret = parse_delete_statement ();
  else if (t && t->type == tok_keyword && t->content == "break")
    ret = parse_break_statement ();
  else if (t && t->type == tok_keyword && t->content == "continue")
    ret = parse_continue_statement ();
  else if (t && t->type == tok_keyword && t->content == "next")
    ret = parse_next_statement ();
  else if (t && (t->type == tok_operator || // expressions are flexible
                 t->type == tok_identifier ||
                 t->type == tok_number ||
                 t->type == tok_string ||
                 t->type == tok_embedded ))
    ret = parse_expr_statement ();
  // XXX: consider generally accepting tok_embedded here too
  else
    throw PARSE_ERROR (_("expected statement"));

  // Squash "empty" trailing colons after any "non-block-like" statement.
  t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    {
      swallow (); // Silently eat trailing ; after statement
    }

  return ret;
}

void
parser::parse_private (vector <vardecl*>& globals, vector<probe*>& probes,
                       string const & fname, vector<functiondecl*>& functions)
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "private"))
    throw PARSE_ERROR (_("expected 'private'"));
  swallow ();
  t = next ();
  if (t->type == tok_keyword && t->content == "function")
  {
    swallow ();
    context = con_function;
    do_parse_functiondecl(functions, t, fname, true);
  }
  else if (t->type == tok_keyword && t->content == "global")
  {
    swallow ();
    context = con_global;
    t = next ();
    if (! (t->type == tok_identifier))
      throw PARSE_ERROR (_("expected identifier"));
    do_parse_global(globals, probes, fname, t, true);
  }
  // The `private <identifier>` is an acceptable shorthand
  // for `private global <identifier>` per above.
  else if (t->type == tok_identifier)
  {
    context = con_global;
    do_parse_global(globals, probes, fname, t, true);
  }
  else
    throw PARSE_ERROR (_("expected 'function' or identifier"));
}

void
parser::parse_global (vector <vardecl*>& globals, vector<probe*>& probes,
                      string const & fname)
{
  const token* t0 = next ();
  if (! (t0->type == tok_keyword && t0->content == "global"))
    throw PARSE_ERROR (_("expected 'global' or 'private'"));
  swallow ();
  do_parse_global(globals, probes, fname, 0, false);
}

void
parser::do_parse_global (vector <vardecl*>& globals, vector<probe*>&,
                         string const & fname, const token* t0, bool priv)
{
  bool iter0 = true;
  const token* t;
  while (1)
    {
      t = (iter0 && priv) ? t0 : next ();
      iter0 = false;
      if (! (t->type == tok_identifier))
        throw PARSE_ERROR (_("expected identifier"));

      string gname = "__global_" + string(t->content);
      string pname = "__private_" + detox_path(fname) + string(t->content);
      string name = priv ? pname : gname;

      for (unsigned i=0; i<globals.size(); i++)
      {
	if (globals[i]->name == name)
	  throw PARSE_ERROR (_("duplicate global name"));
        if ((globals[i]->name == gname) || (globals[i]->name == pname))
          throw PARSE_ERROR (_("global versus private variable declaration conflict"));
      }

      vardecl* d = new vardecl;
      d->unmangled_name = t->content;
      d->name = name;
      d->tok = t;
      d->systemtap_v_conditional = systemtap_v_seen;
      globals.push_back (d);

      t = peek ();

      if(t && t->type == tok_operator && t->content == "%") //wrapping
        {
          d->wrap = true;
          swallow ();
          t = peek();
        }

      if (t && t->type == tok_operator && t->content == "[") // array size
	{
	  int64_t size;
	  swallow ();
	  expect_number(size);
	  if (size <= 0 || size > 1000000) // arbitrary max
	    throw PARSE_ERROR(_("array size out of range"));
	  d->maxsize = (int)size;
	  expect_known(tok_operator, "]");
	  t = peek ();
	}

      if (t && t->type == tok_operator && t->content == "=") // initialization
	{
	  if (!d->compatible_arity(0))
	    throw PARSE_ERROR(_("only scalar globals can be initialized"));
	  d->set_arity(0, t);
	  next (); // Don't swallow, set_arity() used the peeked token.
	  d->init = parse_literal ();
	  d->type = d->init->type;
	  t = peek ();
	}

      if (t && t->type == tok_operator && t->content == ";") // termination
	{
	  swallow ();
	  break;
	}

      if (t && t->type == tok_operator && t->content == ",") // next global
	{
	  swallow ();
	  continue;
	}
      else
	break;
    }
}

void
parser::parse_functiondecl (vector<functiondecl*>& functions,
                            string const & fname)
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "function"))
    throw PARSE_ERROR (_("expected 'function'"));
  swallow ();
  do_parse_functiondecl(functions, t, fname, false);
}

void
parser::do_parse_functiondecl (vector<functiondecl*>& functions, const token* t,
                               string const & fname, bool priv)
{
  t = next ();
  if (! (t->type == tok_identifier)
      && ! (t->type == tok_keyword
	    && (t->content == "string" || t->content == "long")))
    throw PARSE_ERROR (_("expected identifier"));

  string gname = "__global_" + string(t->content);
  string pname = "__private_" + detox_path(fname) + string(t->content);
  string name = priv ? pname : gname;
  name += "__overload_" + lex_cast(session.overload_count[t->content]++);

  functiondecl *fd = new functiondecl ();
  fd->unmangled_name = t->content;
  fd->name = name;
  fd->tok = t;

  t = next ();
  if (t->type == tok_operator && t->content == ":")
    {
      swallow ();
      t = next ();
      if (t->type == tok_keyword && t->content == "string")
	fd->type = pe_string;
      else if (t->type == tok_keyword && t->content == "long")
	fd->type = pe_long;
      else throw PARSE_ERROR (_("expected 'string' or 'long'"));
      swallow ();

      t = next ();
    }

  if (! (t->type == tok_operator && t->content == "("))
    throw PARSE_ERROR (_("expected '('"));
  swallow ();

  while (1)
    {
      t = next ();

      // permit zero-argument functions
      if (t->type == tok_operator && t->content == ")")
        {
          swallow ();
          break;
        }
      else if (! (t->type == tok_identifier))
	throw PARSE_ERROR (_("expected identifier"));
      vardecl* vd = new vardecl;
      vd->unmangled_name = vd->name = t->content;

      for (auto it = fd->formal_args.begin() ; it != fd->formal_args.end(); ++it)
	{
	  string param = vd->unmangled_name;
	  if ((*it)->unmangled_name == param)
	    throw PARSE_ERROR(_("duplicate parameter names"));
	}

      vd->tok = t;
      fd->formal_args.push_back (vd);
      fd->systemtap_v_conditional = systemtap_v_seen;

      t = next ();

      if (t->type == tok_operator && t->content == ":")
	{
	  swallow ();
	  t = next ();
	  if (t->type == tok_keyword && t->content == "string")
	    vd->type = pe_string;
	  else if (t->type == tok_keyword && t->content == "long")
	    vd->type = pe_long;
	  else throw PARSE_ERROR (_("expected 'string' or 'long'"));
	  swallow ();
	  t = next ();
	}
      if (t->type == tok_operator && t->content == ")")
	{
	  swallow ();
	  break;
	}      
      if (t->type == tok_operator && t->content == ",")
	{
	  swallow ();
	  continue;
	}
      else
	throw PARSE_ERROR (_("expected ',' or ')'"));
    }

  t = peek();
  if (t->type == tok_operator && t->content == ":")
    {
      swallow();
      literal* literal = parse_literal();
      fd->priority = dynamic_cast<literal_number*>(literal)->value;
      // reserve priority 0 for user script implementation
      if (fd->priority < 1)
        throw PARSE_ERROR (_("specified priority must be > 0"));
      delete literal;
    }
  else if (user_file)
    {
      // allow script file implementation override automatically when
      // priority not specified
      fd->priority = 0;
    }

  t = peek ();
  if (t && t->type == tok_embedded)
    fd->body = parse_embeddedcode ();
  else
    fd->body = parse_stmt_block ();

  functions.push_back (fd);
}

vector<probe_point*>
parser::parse_probe_points()
{
  vector<probe_point*> pps;
  while (1)
    {
      vector<probe_point*> tail = parse_components();
      pps.insert(pps.end(), tail.begin(), tail.end());

      const token* t = peek();
      if (t && t->type == tok_operator && t->content == ",")
        {
          swallow();
          continue;
        }

      if (t && t->type == tok_operator
          && (t->content == "{" || t->content == "=" ||
              t->content == "+="|| t->content == "}"))
        break;

      throw PARSE_ERROR (_("expected one of ', { } = +='"));
    }
  return pps;
}

vector<probe_point*>
parser::parse_components()
{
  vector<probe_point*> pps;
  while (1)
    {
      vector<probe_point*> suffix = parse_component();

      // Cartesian product of components
      if (pps.empty())
        pps = suffix;
      else
        {
          assert(!suffix.empty());
          vector<probe_point*> product;
          for (unsigned i = 0; i < pps.size(); i++)
            {
              if (pps[i]->optional || pps[i]->sufficient || pps[i]->condition)
                throw PARSE_ERROR (_("'?', '!' or condition must only be specified in suffix"),
                                   pps[i]->components[0]->tok);
              for (unsigned j = 0; j < suffix.size(); j++)
                {
                  probe_point* pp = new probe_point;
                  pp->components.insert(pp->components.end(),
                                        pps[i]->components.begin(),
                                        pps[i]->components.end());
                  pp->components.insert(pp->components.end(),
                                        suffix[j]->components.begin(),
                                        suffix[j]->components.end());
                  pp->optional = suffix[j]->optional;
                  pp->sufficient = suffix[j]->sufficient;
                  if (auto_path)
                    pp->auto_path = suffix[j]->auto_path;
                  pp->condition = suffix[j]->condition;
                  product.push_back(pp);
                }
            }
          for (unsigned i = 0; i < pps.size(); i++) delete pps[i];
          for (unsigned i = 0; i < suffix.size(); i++) delete suffix[i];
          pps = product;
        }

      const token* t = peek();
      if (t && t->type == tok_operator && t->content == ".")
        {
          swallow ();
          continue;
        }

      // We only fall through here at the end of 	a probe point (past
      // all the dotted/parametrized components).

      if (t && t->type == tok_operator &&
          (t->content == "?" || t->content == "!"))
        {
          for (unsigned i = 0; i < pps.size(); i++)
            {
              if (pps[i]->optional || pps[i]->sufficient)
                throw PARSE_ERROR (_("'?' or '!' respecified"));
              pps[i]->optional = true;
              if (t->content == "!") pps[i]->sufficient = true;
            }
          // NB: sufficient implies optional
          swallow ();
          t = peek ();
          // fall through
        }

      if (t && t->type == tok_keyword && t->content == "if")
        {
          swallow ();
          t = peek ();
          if (!(t && t->type == tok_operator && t->content == "("))
            throw PARSE_ERROR (_("expected '('"));
          swallow ();

          expression* e = parse_expression();
          for (unsigned i = 0; i < pps.size(); i++)
            {
              if (pps[i]->condition != 0)
                throw PARSE_ERROR (_("condition respecified"));
              pps[i]->condition = e;
            }

          t = peek ();
          if (!(t && t->type == tok_operator && t->content == ")"))
            throw PARSE_ERROR (_("expected ')'"));
          swallow ();
        }

      break;
    }
  return pps;
}

vector<probe_point*>
parser::parse_component()
{
  const token* t = next ();
  if (! (t->type == tok_identifier
         // we must allow ".return" and ".function", which are keywords
         || t->type == tok_keyword
         // we must allow "*", due to being an operator
         || (t->type == tok_operator && (t->content == "*" || t->content == "{"))))
    throw PARSE_ERROR (_("expected identifier or '*' or '{'"));

  if (t && t->type == tok_operator && t->content == "{")
    {
      swallow();
      vector<probe_point*> pps = parse_probe_points();
      t = peek();
      if (!(t && t->type == tok_operator && t->content == "}"))
        throw PARSE_ERROR (_("expected '}'"));
      swallow();
      return pps;
    }
  else
    {
      // loop which reconstitutes an identifier with wildcards
      string content = t->content;
      bool changed_p = false;
      while (1)
        {
          const token* u = peek();
          if (u == NULL)
            break;
          // ensure pieces of the identifier are adjacent:
          if (input.ate_whitespace)
            break;
          // ensure pieces of the identifier are valid:
          if (! (u->type == tok_identifier
                 // we must allow arbitrary keywords with a wildcard
                 || u->type == tok_keyword
                 // we must allow "*", due to being an operator
                 || (u->type == tok_operator && u->content == "*")))
            break;

          // append u to t
          content = content + (string)u->content;
          changed_p = true;

          // consume u
          swallow ();
        }

      if (changed_p)
        {
          // We've already swallowed the first token and we're not
          // putting it back; no one else has a copy; so we can
          // safely overwrite its content and reuse it.
          const_cast<token*>(t)->content = content;
        }

      probe_point::component* c = new probe_point::component;
      c->functor = t->content;
      c->tok = t;
      vector<probe_point*> pps;
      probe_point* pp = new probe_point;
      if (auto_path)
        pp->auto_path = input_name;
      pp->components.push_back(c);
      pps.push_back(pp);
      // NB we may add c->arg soon

      t = peek ();

      // consume optional parameter
      if (t && t->type == tok_operator && t->content == "(")
        {
          swallow (); // consume "("
          c->arg = parse_literal ();

          t = next ();
          if (! (t->type == tok_operator && t->content == ")"))
            throw PARSE_ERROR (_("expected ')'"));
          swallow ();
        }

      return pps;
    }
}

literal_string*
parser::consume_string_literals(const token *t)
{
  literal_string *ls = new literal_string (t->content);

  // PR11208: check if the next token is also a string literal;
  // auto-concatenate it.  This is complicated to the extent that we
  // need to skip intermediate whitespace.
  //
  // NB for versions prior to 2.0: but don't skip over intervening comments
  string concat;
  bool p_concat = false;
  const token *n = peek();
  while (n != NULL && n->type == tok_string
         && ! (!input.has_version("2.0") && input.ate_comment))
    {
      if (!p_concat)
        {
          concat = t->content;
          p_concat = true;
        }
      concat.append(n->content.data(), n->content.size());
      next(); // consume the token
      n = peek();
    }
  if (p_concat)
    ls->value = concat;
  return ls;
}


// Parse a string literal and perform backslash escaping on the contents:
literal_string*
parser::parse_literal_string ()
{
  const token* t = next ();
  literal_string* l;
  if (t->type == tok_string)
    l = consume_string_literals (t);
  else
    throw PARSE_ERROR (_("expected literal string"));

  l->tok = t;
  return l;
}


literal*
parser::parse_literal ()
{
  const token* t = next ();
  literal* l;
  if (t->type == tok_string)
    {
      l = consume_string_literals (t);
    }
  else
    {
      bool neg = false;
      if (t->type == tok_operator && t->content == "-")
	{
	  neg = true;
	  swallow ();
	  t = next ();
	}

      if (t->type == tok_number)
	{
          const string& s = t->content;
	  const char* startp = s.c_str ();
	  char* endp = (char*) startp;

	  // NB: we allow controlled overflow from LLONG_MIN .. ULLONG_MAX
	  // Actually, this allows all the way from -ULLONG_MAX to ULLONG_MAX,
	  // since the lexer only gives us positive digit strings, but we'll
	  // limit it to LLONG_MIN when a '-' operator is fed into the literal.
	  errno = 0;
	  long long value = (long long) strtoull (startp, & endp, 0);
	  if (errno == ERANGE || errno == EINVAL || *endp != '\0'
	      || (neg && (unsigned long long) value > 9223372036854775808ULL)
	      || (unsigned long long) value > 18446744073709551615ULL
	      || value < -9223372036854775807LL-1)
	    throw PARSE_ERROR (_("number invalid or out of range"));

	  if (neg)
	    value = -value;

	  l = new literal_number (value);
	}
      else
	throw PARSE_ERROR (_("expected literal string or number"));
    }

  l->tok = t;
  return l;
}


if_statement*
parser::parse_if_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "if"))
    throw PARSE_ERROR (_("expected 'if'"));
  if_statement* s = new if_statement;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw PARSE_ERROR (_("expected '('"));
  swallow ();

  s->condition = parse_expression ();

  t = next ();
  if (! (t->type == tok_operator && t->content == ")"))
    throw PARSE_ERROR (_("expected ')'"));
  swallow ();

  s->thenblock = parse_statement ();

  t = peek ();
  if (t && t->type == tok_keyword && t->content == "else")
    {
      swallow ();
      s->elseblock = parse_statement ();
    }
  else
    s->elseblock = 0; // in case not otherwise initialized

  return s;
}


expr_statement*
parser::parse_expr_statement ()
{
  expr_statement *es = new expr_statement;
  const token* t = peek ();
  if (t == NULL)
    throw PARSE_ERROR (_("expression statement expected"));
  // Copy, we only peeked, parse_expression might swallow.
  es->tok = new token (*t);
  es->value = parse_expression ();
  return es;
}


return_statement*
parser::parse_return_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "return"))
    throw PARSE_ERROR (_("expected 'return'"));
  if (context != con_function)
    throw PARSE_ERROR (_("found 'return' not in function context"));
  return_statement* s = new return_statement;
  s->tok = t;
  s->value = parse_expression ();
  return s;
}


delete_statement*
parser::parse_delete_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "delete"))
    throw PARSE_ERROR (_("expected 'delete'"));
  delete_statement* s = new delete_statement;
  s->tok = t;
  s->value = parse_expression ();
  return s;
}


next_statement*
parser::parse_next_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "next"))
    throw PARSE_ERROR (_("expected 'next'"));
  next_statement* s = new next_statement;
  s->tok = t;
  return s;
}


break_statement*
parser::parse_break_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "break"))
    throw PARSE_ERROR (_("expected 'break'"));
  break_statement* s = new break_statement;
  s->tok = t;
  return s;
}


continue_statement*
parser::parse_continue_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "continue"))
    throw PARSE_ERROR (_("expected 'continue'"));
  continue_statement* s = new continue_statement;
  s->tok = t;
  return s;
}


for_loop*
parser::parse_for_loop ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "for"))
    throw PARSE_ERROR (_("expected 'for'"));
  for_loop* s = new for_loop;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw PARSE_ERROR (_("expected '('"));
  swallow ();

  // initializer + ";"
  t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    {
      s->init = 0;
      swallow ();
    }
  else
    {
      s->init = parse_expr_statement ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ";"))
	throw PARSE_ERROR (_("expected ';'"));
      swallow ();
    }

  // condition + ";"
  t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    {
      literal_number* l = new literal_number(1);
      s->cond = l;
      s->cond->tok = next ();
    }
  else
    {
      s->cond = parse_expression ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ";"))
	throw PARSE_ERROR (_("expected ';'"));
      swallow ();
    }

  // increment + ")"
  t = peek ();
  if (t && t->type == tok_operator && t->content == ")")
    {
      s->incr = 0;
      swallow ();
    }
  else
    {
      s->incr = parse_expr_statement ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ")"))
	throw PARSE_ERROR (_("expected ')'"));
      swallow ();
    }

  // block
  s->block = parse_statement ();

  return s;
}


for_loop*
parser::parse_while_loop ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "while"))
    throw PARSE_ERROR (_("expected 'while'"));
  for_loop* s = new for_loop;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw PARSE_ERROR (_("expected '('"));
  swallow ();

  // dummy init and incr fields
  s->init = 0;
  s->incr = 0;

  // condition
  s->cond = parse_expression ();

  t = next ();
  if (! (t->type == tok_operator && t->content == ")"))
    throw PARSE_ERROR (_("expected ')'"));
  swallow ();

  // block
  s->block = parse_statement ();

  return s;
}


foreach_loop*
parser::parse_foreach_loop ()
{
  const token* t = next ();
  if (! (t->type == tok_keyword && t->content == "foreach"))
    throw PARSE_ERROR (_("expected 'foreach'"));
  foreach_loop* s = new foreach_loop;
  s->tok = t;
  s->sort_direction = 0;
  s->sort_aggr = sc_none;
  s->value = NULL;
  s->limit = NULL;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw PARSE_ERROR (_("expected '('"));
  swallow ();

  symbol* lookahead_sym = NULL;
  int lookahead_sort = 0;

  t = peek ();
  if (t && t->type == tok_identifier)
    {
      next ();
      lookahead_sym = new symbol;
      lookahead_sym->tok = t;
      lookahead_sym->name = t->content;

      t = peek ();
      if (t && t->type == tok_operator &&
	  (t->content == "+" || t->content == "-"))
	{
	  lookahead_sort = (t->content == "+") ? 1 : -1;
	  swallow ();
	}

      t = peek ();
      if (t && t->type == tok_operator && t->content == "=")
	{
	  swallow ();
	  s->value = lookahead_sym;
	  if (lookahead_sort)
	    {
	      s->sort_direction = lookahead_sort;
	      s->sort_column = 0;
	    }
	  lookahead_sym = NULL;
	}
    }

  // see also parse_array_in

  bool parenthesized = false;
  t = peek ();
  if (!lookahead_sym && t && t->type == tok_operator && t->content == "[")
    {
      swallow ();
      parenthesized = true;
    }

  if (lookahead_sym)
    {
      s->indexes.push_back (lookahead_sym);
      if (lookahead_sort)
	{
	  s->sort_direction = lookahead_sort;
	  s->sort_column = 1;
	}
      lookahead_sym = NULL;
    }
  else while (1)
    {
      t = next ();
      if (! (t->type == tok_identifier))
        throw PARSE_ERROR (_("expected identifier"));
      symbol* sym = new symbol;
      sym->tok = t;
      sym->name = t->content;
      s->indexes.push_back (sym);

      t = peek ();
      if (t && t->type == tok_operator &&
	  (t->content == "+" || t->content == "-"))
	{
	  if (s->sort_direction)
	    throw PARSE_ERROR (_("multiple sort directives"));
	  s->sort_direction = (t->content == "+") ? 1 : -1;
	  s->sort_column = s->indexes.size();
	  swallow ();
	}

      if (parenthesized)
        {
          t = peek ();
          if (t && t->type == tok_operator && t->content == ",")
            {
              swallow ();
              continue;
            }
          else if (t && t->type == tok_operator && t->content == "]")
            {
              swallow ();
              break;
            }
          else
            throw PARSE_ERROR (_("expected ',' or ']'"));
        }
      else
        break; // expecting only one expression
    }

  t = next ();
  if (! (t->type == tok_keyword && t->content == "in"))
    throw PARSE_ERROR (_("expected 'in'"));
  swallow ();

  s->base = parse_indexable();

  // check if there was an array slice that was specified
  t = peek();
  if (t && t->type == tok_operator && t->content == "[")
    {
      swallow();
      while (1)
        {
          t = peek();
          if (t && t->type == tok_operator && t->content == "*")
            {
              swallow();
              s->array_slice.push_back (NULL);
            }
          else
            s->array_slice.push_back (parse_expression());

          t = peek ();
          if (t && t->type == tok_operator && t->content == ",")
            {
              swallow ();
              continue;
            }
          else if (t && t->type == tok_operator && t->content == "]")
            {
              swallow ();
              break;
            }
          else
            throw PARSE_ERROR (_("expected ',' or ']'"));
        }
    }


  // check for atword, see also expect_ident_or_atword,
  t = peek ();
  if (t && t->type == tok_operator && t->content[0] == '@')
    {
      if (t->content == "@avg") s->sort_aggr = sc_average;
      else if (t->content == "@min") s->sort_aggr = sc_min;
      else if (t->content == "@max") s->sort_aggr = sc_max;
      else if (t->content == "@count") s->sort_aggr = sc_count;
      else if (t->content == "@sum") s->sort_aggr = sc_sum;
      else if (t->content == "@variance") s->sort_aggr = sc_variance;
      else throw PARSE_ERROR(_("expected statistical operation"));
      swallow();

      t = peek ();
      if (! (t && t->type == tok_operator && (t->content == "+" || t->content == "-")))
        throw PARSE_ERROR(_("expected sort directive"));
    } 

  t = peek ();
  if (t && t->type == tok_operator &&
      (t->content == "+" || t->content == "-"))
    {
      if (s->sort_direction)
	throw PARSE_ERROR (_("multiple sort directives"));
      s->sort_direction = (t->content == "+") ? 1 : -1;
      s->sort_column = 0;
      swallow ();
    }

  t = peek ();
  if (tok_is(t, tok_keyword, "limit"))
    {
      swallow ();			// get past the "limit"
      s->limit = parse_expression ();
    }

  t = next ();
  if (! (t->type == tok_operator && t->content == ")"))
    throw PARSE_ERROR ("expected ')'");
  swallow ();

  s->block = parse_statement ();
  return s;
}


expression*
parser::parse_expression ()
{
  return parse_assignment ();
}


expression*
parser::parse_assignment ()
{
  expression* op1 = parse_ternary ();

  const token* t = peek ();
  // right-associative operators
  if (t && t->type == tok_operator
      && (t->content == "=" ||
	  t->content == "<<<" ||
	  t->content == "+=" ||
	  t->content == "-=" ||
	  t->content == "*=" ||
	  t->content == "/=" ||
	  t->content == "%=" ||
	  t->content == "<<=" ||
	  t->content == ">>=" ||
	  t->content == "&=" ||
	  t->content == "^=" ||
	  t->content == "|=" ||
	  t->content == ".=" ||
	  false))
    {
      // NB: lvalueness is checked during elaboration / translation
      assignment* e = new assignment;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_expression ();
      op1 = e;
    }

  return op1;
}


expression*
parser::parse_ternary ()
{
  expression* op1 = parse_logical_or ();

  const token* t = peek ();
  if (t && t->type == tok_operator && t->content == "?")
    {
      ternary_expression* e = new ternary_expression;
      e->tok = t;
      e->cond = op1;
      next ();
      e->truevalue = parse_expression (); // XXX

      t = next ();
      if (! (t->type == tok_operator && t->content == ":"))
        throw PARSE_ERROR (_("expected ':'"));
      swallow ();

      e->falsevalue = parse_expression (); // XXX
      return e;
    }
  else
    return op1;
}


expression*
parser::parse_logical_or ()
{
  expression* op1 = parse_logical_and ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "||")
    {
      logical_or_expr* e = new logical_or_expr;
      e->tok = t;
      e->op = t->content;
      e->left = op1;
      next ();
      e->right = parse_logical_and ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_logical_and ()
{
  expression* op1 = parse_boolean_or ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "&&")
    {
      logical_and_expr *e = new logical_and_expr;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_boolean_or ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_boolean_or ()
{
  expression* op1 = parse_boolean_xor ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "|")
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_boolean_xor ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_boolean_xor ()
{
  expression* op1 = parse_boolean_and ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "^")
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_boolean_and ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_boolean_and ()
{
  expression* op1 = parse_array_in ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "&")
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_array_in ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_array_in ()
{
  // This is a very tricky case.  All these are legit expressions:
  // "a in b"  "a+0 in b" "[a,b] in c" "[c,(d+0)] in b"
  vector<expression*> indexes;
  bool parenthesized = false;

  const token* t = peek ();
  if (t && t->type == tok_operator && t->content == "[")
    {
      swallow ();
      parenthesized = true;
    }

  while (1)
    {
      t = peek();
      if (t && t->type == tok_operator && t->content == "*" && parenthesized)
        {
          swallow();
          indexes.push_back(NULL);
        }
      else
        {
          expression* op1 = parse_comparison_or_regex_query ();
          indexes.push_back (op1);
        }

      if (parenthesized)
        {
          const token* t = peek ();
          if (t && t->type == tok_operator && t->content == ",")
            {
              swallow ();
              continue;
            }
          else if (t && t->type == tok_operator && t->content == "]")
            {
              swallow ();
              break;
            }
          else
            throw PARSE_ERROR (_("expected ',' or ']'"));
        }
      else
        break; // expecting only one expression
    }

  t = peek ();
  if (t && t->type == tok_keyword && t->content == "in")
    {
      array_in *e = new array_in;
      e->tok = t;
      next ();

      arrayindex* a = new arrayindex;
      a->indexes = indexes;
      a->base = parse_indexable();
      a->tok = a->base->tok;
      e->operand = a;
      return e;
    }
  else if (indexes.size() == 1) // no "in" - need one expression only
    return indexes[0];
  else
    throw PARSE_ERROR (_("unexpected comma-separated expression list"));
}


expression*
parser::parse_comparison_or_regex_query ()
{
  expression* op1 = parse_shift ();

  // XXX precedence -- perhaps a =~ b == c =~ d --> (a =~ b) == (c =~ d) ?
  const token *t = peek();
  if (t && t->type == tok_operator
      && (t->content == "=~" ||
          t->content == "!~"))
    {
      regex_query* r = new regex_query;
      r->left = op1;
      r->op = t->content;
      r->tok = t;
      next ();
      r->right = parse_literal_string();
      op1 = r;
      t = peek ();
    }
  else while (t && t->type == tok_operator
      && (t->content == ">" ||
          t->content == "<" ||
          t->content == "==" ||
          t->content == "!=" ||
          t->content == "<=" ||
          t->content == ">="))
    {
      comparison* e = new comparison;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_shift ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_shift ()
{
  expression* op1 = parse_concatenation ();

  const token* t = peek ();
  while (t && t->type == tok_operator &&
         (t->content == "<<" || t->content == ">>" || t->content == ">>>"))
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_concatenation ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_concatenation ()
{
  expression* op1 = parse_additive ();

  const token* t = peek ();
  // XXX: the actual awk string-concatenation operator is *whitespace*.
  // I don't know how to easily to model that here.
  while (t && t->type == tok_operator && t->content == ".")
    {
      concatenation* e = new concatenation;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_additive ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_additive ()
{
  expression* op1 = parse_multiplicative ();

  const token* t = peek ();
  while (t && t->type == tok_operator
      && (t->content == "+" || t->content == "-"))
    {
      binary_expression* e = new binary_expression;
      e->op = t->content;
      e->left = op1;
      e->tok = t;
      next ();
      e->right = parse_multiplicative ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_multiplicative ()
{
  expression* op1 = parse_unary ();

  const token* t = peek ();
  while (t && t->type == tok_operator
      && (t->content == "*" || t->content == "/" || t->content == "%"))
    {
      binary_expression* e = new binary_expression;
      e->op = t->content;
      e->left = op1;
      e->tok = t;
      next ();
      e->right = parse_unary ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_unary ()
{
  const token* t = peek ();
  if (t && t->type == tok_operator
      && (t->content == "+" ||
          t->content == "-" ||
          t->content == "!" ||
          t->content == "~" ||
          false))
    {
      unary_expression* e = new unary_expression;
      e->op = t->content;
      e->tok = t;
      next ();
      e->operand = parse_unary ();
      return e;
    }
  else
    return parse_crement ();
}


expression*
parser::parse_crement () // as in "increment" / "decrement"
{
  // NB: Ideally, we'd parse only a symbol as an operand to the
  // *crement operators, instead of a general expression value.  We'd
  // need more complex lookahead code to tell apart the postfix cases.
  // So we just punt, and leave it to pass-3 to signal errors on
  // cases like "4++".

  const token* t = peek ();
  if (t && t->type == tok_operator
      && (t->content == "++" || t->content == "--"))
    {
      pre_crement* e = new pre_crement;
      e->op = t->content;
      e->tok = t;
      next ();
      e->operand = parse_dwarf_value ();
      return e;
    }

  // post-crement or non-crement
  expression *op1 = parse_dwarf_value ();

  t = peek ();
  if (t && t->type == tok_operator
      && (t->content == "++" || t->content == "--"))
    {
      post_crement* e = new post_crement;
      e->op = t->content;
      e->tok = t;
      next ();
      e->operand = op1;
      return e;
    }
  else
    return op1;
}


expression*
parser::parse_dwarf_value ()
{
  expression* expr = NULL;
  target_symbol* tsym = NULL;

  // With '&' we'll definitely be making a target symbol of some sort
  const token* addrtok = peek_op ("&") ? next () : NULL;
  bool addressof = (addrtok != NULL);

  // First try target_symbol types: $var, @cast, and @var.
  const token* t = peek ();
  if (t && t->type == tok_identifier && t->content[0] == '$')
    expr = tsym = parse_target_symbol ();
  else if (tok_is (t, tok_operator, "@cast"))
    expr = tsym = parse_cast_op ();
  else if (tok_is (t, tok_operator, "@var"))
    expr = tsym = parse_atvar_op ();
  else if (addressof && !input.has_version("2.6"))
    // '&' on old version only allowed specific target_symbol types
    throw PARSE_ERROR (_("expected @cast, @var or $var"));
  else
    // Otherwise just get a plain value of any sort.
    expr = parse_value ();

  // If we had '&' or see any target suffixes, that forces a target_symbol.
  // For compatibility, we only do this starting with 2.6.
  if (!tsym && (addressof || peek_target_symbol_components ())
      && input.has_version("2.6"))
    {
      autocast_op *cop = new autocast_op;
      cop->tok = addrtok ?: peek ();
      cop->operand = expr;
      expr = tsym = cop;
    }

  if (tsym)
    {
      // Parse the rest of any kind of target symbol
      tsym->addressof = addressof;
      parse_target_symbol_components (tsym);
    }

  return expr;
}


expression*
parser::parse_value ()
{
  const token* t = peek ();
  if (! t)
    throw PARSE_ERROR (_("expected value"));

  if (t->type == tok_embedded)
    {
      if (! privileged)
        throw PARSE_ERROR (_("embedded expression code in unprivileged script; need stap -g"), false);

      embedded_expr *e = new embedded_expr;
      e->tok = t;
      e->code = t->content;
      next ();
      return e;
    }

  if (t->type == tok_operator && t->content == "(")
    {
      swallow ();
      expression* e = parse_expression ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ")"))
        throw PARSE_ERROR (_("expected ')'"));
      swallow ();
      return e;
    }
  else if (t->type == tok_identifier
           || (t->type == tok_operator && t->content[0] == '@'))
    return parse_symbol ();
  else
    return parse_literal ();
}


const token *
parser::parse_hist_op_or_bare_name (hist_op *&hop, interned_string &name)
{
  hop = NULL;
  const token* t = expect_ident_or_atword (name);
  if (name == "@hist_linear" || name == "@hist_log")
    {
      hop = new hist_op;
      if (name == "@hist_linear")
	hop->htype = hist_linear;
      else if (name == "@hist_log")
	hop->htype = hist_log;
      hop->tok = t;
      expect_op("(");
      hop->stat = parse_expression ();
      int64_t tnum;
      if (hop->htype == hist_linear)
	{
	  for (size_t i = 0; i < 3; ++i)
	    {
	      expect_op (",");
	      expect_number (tnum);
	      hop->params.push_back (tnum);
	    }
	}
      expect_op(")");
    }
  return t;
}


indexable*
parser::parse_indexable ()
{
  hist_op *hop = NULL;
  interned_string name;
  const token *tok = parse_hist_op_or_bare_name(hop, name);
  if (hop)
    return hop;
  else
    {
      symbol* sym = new symbol;
      sym->name = name;
      sym->tok = tok;
      return sym;
    }
}


// var, indexable[index], func(parms), printf("...", ...),
// @defined, @entry, @stat_op(stat)
expression* parser::parse_symbol ()
{
  hist_op *hop = NULL;
  symbol *sym = NULL;
  interned_string name;
  unsigned max_params = 0;
  const token *t = parse_hist_op_or_bare_name(hop, name);

  if (!hop)
    {
      // If we didn't get a hist_op, then we did get an identifier. We can
      // now scrutinize this identifier for the various magic forms of identifier
      // (printf, @stat_op...)

      // NB: PR11343: @defined() is not incompatible with earlier versions
      // of stap, so no need to check session.compatible for 1.2
      if (name == "@defined")
        return parse_defined_op (t);

      if (name == "@const")
        return parse_const_op (t);

      if (name == "@entry")
        return parse_entry_op (t);

      if (name == "@perf")
        return parse_perf_op (t);

      if (name.size() > 0 && name[0] == '@')
	{
	  stat_op *sop = new stat_op;
	  if (name == "@avg")
	    sop->ctype = sc_average;
	  else if (name == "@variance")
	    sop->ctype = sc_variance, max_params = 1;
	  else if (name == "@count")
	    sop->ctype = sc_count;
	  else if (name == "@sum")
	    sop->ctype = sc_sum;
	  else if (name == "@min")
	    sop->ctype = sc_min;
	  else if (name == "@max")
	    sop->ctype = sc_max;
	  else
	    throw PARSE_ERROR(_F("unknown operator %s",
                                 name.to_string().c_str()));
	  expect_op("(");
	  sop->tok = t;
	  sop->stat = parse_expression ();

	  while(1)
	    {
	      t = next ();
	      if (t && t->type == tok_operator && t->content == ")")
	        {
	          swallow ();
	          break;
	        }
	        else if (t && t->type == tok_operator && t->content == ",")
	        {
	          if (sop->params.size() >= max_params)
	            throw PARSE_ERROR(_NF("not more than %d parameter allowed",
	                                  "not more than %d parameters allowed",
	                                  max_params+1, max_params+1), t);

	          swallow ();
	          int64_t tnum;
	          expect_number (tnum);
	          sop->params.push_back (tnum);
	        }
	    }
	  return sop;
	}

      else if (print_format *fmt = print_format::create(t))
	{
	  expect_op("(");
	  if ((name == "print" || name == "println" ||
	       name == "sprint" || name == "sprintln") &&
	      (peek_op("@hist_linear") || peek_op("@hist_log")))
	    {
	      // We have a special case where we recognize
	      // print(@hist_foo(bar)) as a magic print-the-histogram
	      // construct. This is sort of gross but it avoids
	      // promoting histogram references to typeful
	      // expressions.

	      hop = NULL;
	      t = parse_hist_op_or_bare_name(hop, name);
	      assert(hop);

	      // It is, sadly, possible that even while parsing a
	      // hist_op, we *mis-guessed* and the user wishes to
	      // print(@hist_op(foo)[bucket]), a scalar. In that case
	      // we must parse the arrayindex and print an expression.
	      //
	      // XXX: This still fails if the arrayindex is part of a
	      // larger expression.  To really handle everything, we'd
	      // need to push back all the hist tokens start over.

	      if (!peek_op ("["))
		fmt->hist = hop;
	      else
		{
		  // This is simplified version of the
		  // multi-array-index parser below, because we can
		  // only ever have one index on a histogram anyways.
		  expect_op("[");
		  struct arrayindex* ai = new arrayindex;
		  ai->tok = t;
		  ai->base = hop;
		  ai->indexes.push_back (parse_expression ());
		  expect_op("]");
		  fmt->args.push_back(ai);

		  // Consume any subsequent arguments.
		  while (!peek_op(")"))
		    {
		      // ')' is not possible here but we want to output a nicer
		      // parser error message.
		      (void) expect_op_any ({",", ")"});
		      expression *e = parse_expression ();
		      fmt->args.push_back(e);
		    }
		}
	    }
	  else
	    {
	      int min_args = 0;
	      bool consumed_arg = false;
	      if (fmt->print_with_format)
		{
		  // Consume and convert a format string. Agreement between the
		  // format string and the arguments is postponed to the
		  // typechecking phase.
                  literal_string* ls = parse_literal_string();
		  fmt->raw_components = ls->value;
                  delete ls;
                  fmt->components = print_format::string_to_components (fmt->raw_components);
		  consumed_arg = true;
		}
	      else if (fmt->print_with_delim)
		{
		  // Consume a delimiter to separate arguments.
                  literal_string* ls = parse_literal_string();
		  fmt->delimiter = ls->value;
                  delete ls;
		  consumed_arg = true;
		  min_args = 2; // so that the delim is used at least once
		}
	      else if (!fmt->print_with_newline)
		{
		  // If we are not printing with a format string, nor with a
		  // delim, nor with a newline, then it's either print() or
		  // sprint(), both of which require at least one argument (of
		  // any type).
		  min_args = 1;
		}

	      // Consume any subsequent arguments.
	      while (min_args || !peek_op (")"))
		{
                  // ')' is not possible here but we want to output a nicer
                  // parser error message.
		  if (consumed_arg)
		    (void) expect_op_any({",", ")"});
		  expression *e = parse_expression ();
		  fmt->args.push_back(e);
		  consumed_arg = true;
		  if (min_args)
		    --min_args;
		}
	    }
	  expect_op(")");
	  return fmt;
	}

      else if (peek_op ("(")) // function call
	{
	  swallow ();
	  struct functioncall* f = new functioncall;
	  f->tok = t;
	  f->function = name;
	  // Allow empty actual parameter list
	  if (peek_op (")"))
	    {
	      swallow ();
	      return f;
	    }
	  while (1)
	    {
	      f->args.push_back (parse_expression ());
              interned_string op = expect_op_any({")", ","});
              if (op == ")")
                break;
              else if (op == ",")
                continue;
	    }
	  return f;
	}

      else
	{
	  sym = new symbol;
	  sym->name = name;
	  sym->tok = t;
	}
    }

  // By now, either we had a hist_op in the first place, or else
  // we had a plain word and it was converted to a symbol.

  assert (!hop != !sym); // logical XOR

  // All that remains is to check for array indexing

  if (peek_op ("[")) // array
    {
      swallow ();
      struct arrayindex* ai = new arrayindex;
      ai->tok = t;

      if (hop)
	ai->base = hop;
      else
	ai->base = sym;

      while (1)
        {
          if (peek_op("*"))
            {
              swallow();
              ai->indexes.push_back (NULL);
            }
          else
            ai->indexes.push_back (parse_expression ());
          interned_string op = expect_op_any({"]", ","});
          if (op == "]")
            break;
          else if (op == ",")
            continue;
        }

      return ai;
    }

  // If we got to here, we *should* have a symbol; if we have
  // a hist_op on its own, it doesn't count as an expression,
  // so we throw a parse error.

  if (hop)
    throw PARSE_ERROR(_("base histogram operator where expression expected"), t);

  return sym;
}

// Parse a $var.
target_symbol* parser::parse_target_symbol ()
{
  const token* t = next ();
  if (t->type == tok_identifier && t->content[0]=='$')
    {
      // target_symbol time
      target_symbol *tsym = new target_symbol;
      tsym->tok = t;
      tsym->name = t->content;
      return tsym;
    }

  throw PARSE_ERROR (_("expected $var"));
}


// Parse a @cast.
cast_op* parser::parse_cast_op ()
{
  const token* t = next ();
  if (t->type == tok_operator && t->content == "@cast")
    {
      cast_op *cop = new cast_op;
      cop->tok = t;
      cop->name = t->content;
      expect_op("(");
      cop->operand = parse_expression ();
      expect_op(",");
      expect_unknown(tok_string, cop->type_name);
      if (cop->type_name.empty())
        throw PARSE_ERROR (_("expected non-empty string"));
      if (peek_op (","))
        {
          swallow ();
          expect_unknown(tok_string, cop->module);
        }
      expect_op(")");
      return cop;
    }

  throw PARSE_ERROR (_("expected @cast"));
}


// Parse a @var.
atvar_op* parser::parse_atvar_op ()
{
  const token* t = next ();
  if (t->type == tok_operator && t->content == "@var")
    {
      atvar_op *aop = new atvar_op;
      aop->tok = t;
      aop->name = t->content;
      expect_op("(");
      expect_unknown(tok_string, aop->target_name);
      size_t found_at = aop->target_name.find("@");
      if (found_at != string::npos)
        aop->cu_name = aop->target_name.substr(found_at + 1);
      else
        aop->cu_name = "";
      if (peek_op (","))
        {
          swallow ();
          expect_unknown (tok_string, aop->module);
        }
      else
        aop->module = "";
      expect_op(")");
      return aop;
    }

  throw PARSE_ERROR (_("expected @var"));
}


// Parse a @defined().  Given head token has already been consumed.
expression* parser::parse_defined_op (const token* t)
{
  defined_op* dop = new defined_op;
  dop->tok = t;
  expect_op("(");
  dop->operand = parse_expression ();
  expect_op(")");
  return dop;
}


// Parse a @const().  Given head token has already been consumed.
expression* parser::parse_const_op (const token* t)
{
  if (! privileged)
    throw PARSE_ERROR (_("using @const operator not permitted; need stap -g"),
                       false /* don't skip tokens for parse resumption */);

  interned_string cnst;
  embedded_expr *ee = new embedded_expr;
  ee->tok = t;
  expect_op("(");
  expect_unknown(tok_string, cnst);
  if(cnst.empty())
    throw PARSE_ERROR (_("expected non-empty string"));
  expect_op(")");
  ee->code = string("/* pure */ /* unprivileged */ /* stable */ ") + string(cnst);
  return ee;
}


// Parse a @entry().  Given head token has already been consumed.
expression* parser::parse_entry_op (const token* t)
{
  entry_op* eop = new entry_op;
  eop->tok = t;
  expect_op("(");
  eop->operand = parse_expression ();
  expect_op(")");
  return eop;
}


// Parse a @perf().  Given head token has already been consumed.
expression* parser::parse_perf_op (const token* t)
{
  perf_op* pop = new perf_op;
  pop->tok = t;
  expect_op("(");
  pop->operand = parse_literal_string ();
  if (pop->operand->value == "")
    throw PARSE_ERROR (_("expected non-empty string"));
  expect_op(")");
  return pop;
}


bool
parser::peek_target_symbol_components ()
{
  const token * t = peek ();
  return t &&
    ((t->type == tok_operator && (t->content == "->" || t->content == "["))
     || (t->type == tok_identifier &&
         t->content.find_first_not_of('$') == string::npos));
}

void
parser::parse_target_symbol_components (target_symbol* e)
{
  bool pprint = false;

  // check for pretty-print in the form $foo$
  string base = e->name;
  size_t pprint_pos = base.find_last_not_of('$');
  if (0 < pprint_pos && pprint_pos < base.length() - 1)
    {
      string pprint_val = base.substr(pprint_pos + 1);
      base.erase(pprint_pos + 1);
      e->name = base;
      e->components.push_back (target_symbol::component(e->tok, pprint_val, true));
      pprint = true;
    }

  while (!pprint)
    {
      if (peek_op ("->"))
        {
          const token* t = next();
          interned_string member;
          expect_ident_or_keyword (member);

          // check for pretty-print in the form $foo->$ or $foo->bar$
          pprint_pos = member.find_last_not_of('$');
          interned_string pprint_val;
          if (pprint_pos == string::npos || pprint_pos < member.length() - 1)
            {
              pprint_val = member.substr(pprint_pos + 1);
              member = member.substr(0, pprint_pos + 1);
              pprint = true;
            }

          if (!member.empty())
            e->components.push_back (target_symbol::component(t, member));
          if (pprint)
            e->components.push_back (target_symbol::component(t, pprint_val, true));
        }
      else if (peek_op ("["))
        {
          const token* t = next();
          expression* index = parse_expression();
          literal_number* ln = dynamic_cast<literal_number*>(index);
          if (ln)
            e->components.push_back (target_symbol::component(t, ln->value));
          else
            e->components.push_back (target_symbol::component(t, index));
          expect_op ("]");
        }
      else
        break;
    }

  if (!pprint)
    {
      // check for pretty-print in the form $foo $
      // i.e. as a separate token, esp. for $foo[i]$ and @cast(...)$
      const token* t = peek();
      if (t != NULL && t->type == tok_identifier &&
          t->content.find_first_not_of('$') == string::npos)
        {
          t = next();
          e->components.push_back (target_symbol::component(t, t->content, true));
          pprint = true;
        }
    }

  if (pprint && (peek_op ("->") || peek_op("[")))
    throw PARSE_ERROR(_("-> and [ are not accepted for a pretty-printing variable"));
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
