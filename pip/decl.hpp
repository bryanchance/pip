#pragma once

#include <pip/syntax.hpp>
#include <functional>
#include <unordered_set>

namespace pip
{
  // Kinds of declarations.
  enum decl_kind : int
  {
    dk_program,
    dk_table,
    dk_meter,
  };

  // Kinds of matching strategies.
  enum rule_kind : int
  {
    rk_exact,
    rk_prefix,
    rk_wildcard,
    rk_range,
    rk_expr,
  };

  /// The base class of all declarations. A declaration introduces a named
  /// entity.
  struct decl : cc::node
  {
    decl(decl_kind k, symbol* id)
      : cc::node(k, {}), id(id)
    { }

    void dump();

    symbol* id;

    virtual ~decl() {}
  };

  /// A dataplane program. A dataplane program is defined (primarily) by a
  /// list of match table and their associated actions.
  ///
  /// \todo How do we (or should we?) incorporate other kinds of declarations.
  /// For example, should we declare ports? Should we declare meters? Group
  /// actions? Do we need to process the list of declarations to find e.g., 
  /// the entry table?
  struct program_decl : decl
  {
    program_decl(const decl_seq& ds)
      : decl(dk_program, nullptr), decls(ds)
    { }

    program_decl(decl_seq&& ds)
      : decl(dk_program, nullptr), decls(std::move(ds))
    { }

    ~program_decl()
    { for(auto d : decls) if(d) delete d; }

    /// A list of declarations.
    decl_seq decls;
  };

  /// Represents a match in a table. This is a key/value pair where the key
  /// is a literal expression, and the value is an action list expression.
  /// The rule kind of this object must match that of the table it 
  /// appears in.
  struct rule : cc::node
  {
    rule(rule_kind k, expr* key, const action_seq& val)
      : cc::node(0, {}), kind(k), key(key), acts(val)
    { }

    rule(rule_kind k, expr* key, action_seq&& val)
      : cc::node(0, {}), kind(k), key(key), acts(std::move(val))
    { }

    /// The kind of matching operation.
    rule_kind kind;

    /// The matched key -- a literal.
    expr* key;

    /// The list of actions to be executed.
    action_seq acts;
  };


  /// A lookup table. Match tables are determined by a matching kind, which
  /// prescribes the how keys are found in the table.
  ///
  /// \todo Support general purpose tables, that can include arbitrary forms
  /// of matching (e.g., wildcard and exact). Note an exact match in a wildcard
  /// table is simply one with no don't-care bits. Maybe we don't need to
  /// generalize this so much.
  struct table_decl : decl
  {
    table_decl(symbol* id, rule_kind k, const action_seq& as, const rule_seq& ms)
      : decl(dk_table, id), rule(k), prep(as), rules(ms)
    { }

    table_decl(symbol* id, rule_kind k, action_seq&& as, rule_seq&& ms)
      : decl(dk_table, id), rule(k), prep(std::move(as)), rules(std::move(ms))
    { }

    ~table_decl()
    { for(auto r : rules) if(r) delete r; }

    /// The kind of table.
    rule_kind rule;

    /// The sequence of actions used to form the key for lookup.
    action_seq prep;

    /// The content of the action table.
    rule_seq rules;

    /// A hash table to match keys for exact-match tables
    std::unordered_set<std::uint64_t, std::hash<std::uint64_t>> key_table;
  };

  /// A flow metering device.
  ///
  /// \todo Implement this?
  struct meter_decl : decl
  {
    meter_decl(symbol* id)
      : decl(dk_meter, id)
    { }
  };

// -------------------------------------------------------------------------- //
// Operations

  /// Returns the kind of a declaration.
  inline decl_kind 
  get_kind(const decl* d)
  {
    return static_cast<decl_kind>(d->kind);
  }

} // namespace pip

// -------------------------------------------------------------------------- //
// Casting

namespace cc 
{
  template<>
  struct node_info<pip::program_decl>
  {
    static bool
    has_kind(const node* n) { return get_node_kind(n) == pip::dk_program; }
  };

  template<>
  struct node_info<pip::table_decl>
  {
    static bool
    has_kind(const node* n) { return get_node_kind(n) == pip::dk_table; }
  };

  template<>
  struct node_info<pip::meter_decl>
  {
    static bool
    has_kind(const node* n) { return get_node_kind(n) == pip::dk_meter; }
  };

} // namespace cc
