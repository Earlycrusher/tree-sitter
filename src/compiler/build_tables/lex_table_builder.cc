#include "compiler/build_tables/lex_table_builder.h"
#include <climits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <cwctype>
#include <vector>
#include "compiler/build_tables/lex_conflict_manager.h"
#include "compiler/build_tables/lex_item.h"
#include "compiler/parse_table.h"
#include "compiler/lexical_grammar.h"
#include "compiler/rule.h"

namespace tree_sitter {
namespace build_tables {

using std::iswalpha;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;
using std::unordered_map;
using std::unique_ptr;
using rules::Rule;
using rules::Blank;
using rules::Choice;
using rules::CharacterSet;
using rules::Repeat;
using rules::Symbol;
using rules::Metadata;
using rules::Seq;

class StartingCharacterAggregator {
 public:
  void apply(const Rule &rule) {
    rule.match(
      [this](const Seq &sequence) {
        apply(*sequence.left);
      },

      [this](const rules::Choice &rule) {
        for (const auto &element : rule.elements) {
          apply(element);
        }
      },

      [this](const rules::Repeat &rule) {
        apply(*rule.rule);
      },

      [this](const rules::Metadata &rule) {
        apply(*rule.rule);
      },

      [this](const rules::CharacterSet &rule) {
        result.add_set(rule);
      },

      [this](const rules::Blank) {},

      [](auto) {}
    );
  }

  CharacterSet result;
};

class LexTableBuilderImpl : public LexTableBuilder {
  LexTable lex_table;
  const LexicalGrammar grammar;
  vector<Rule> separator_rules;
  LexConflictManager conflict_manager;
  unordered_map<LexItemSet, LexStateId> lex_state_ids;

  map<Symbol::Index, CharacterSet> following_characters_by_token_index;
  CharacterSet separator_start_characters;
  CharacterSet current_conflict_detection_following_characters;
  Symbol::Index current_conflict_detection_token_index;
  bool current_conflict_value;

 public:
  LexTableBuilderImpl(const LexicalGrammar &grammar) : grammar(grammar) {
    StartingCharacterAggregator separator_character_aggregator;
    for (const auto &rule : grammar.separators) {
      separator_rules.push_back(Repeat{rule});
      separator_character_aggregator.apply(rule);
    }
    separator_rules.push_back(Blank{});
    separator_start_characters = separator_character_aggregator.result;
  }

  LexTable build(ParseTable *parse_table) {
    for (ParseState &parse_state : parse_table->states) {
      parse_state.lex_state_id = add_lex_state(
        item_set_for_terminals(parse_state.terminal_entries)
      );
    }
    mark_fragile_tokens(parse_table);
    remove_duplicate_lex_states(parse_table);
    return lex_table;
  }

  bool detect_conflict(Symbol::Index left, Symbol::Index right,
                       const vector<set<Symbol::Index>> &following_terminals_by_terminal_index) {
    clear();

    StartingCharacterAggregator left_starting_characters;
    StartingCharacterAggregator right_starting_characters;
    left_starting_characters.apply(grammar.variables[left].rule);
    right_starting_characters.apply(grammar.variables[right].rule);
    if (!left_starting_characters.result.intersects(right_starting_characters.result) &&
        !left_starting_characters.result.intersects(separator_start_characters) &&
        !right_starting_characters.result.intersects(separator_start_characters)) {
      return false;
    }

    auto following_characters_entry = following_characters_by_token_index.find(right);
    if (following_characters_entry == following_characters_by_token_index.end()) {
      StartingCharacterAggregator aggregator;
      for (auto following_token_index : following_terminals_by_terminal_index[right]) {
        aggregator.apply(grammar.variables[following_token_index].rule);
      }
      following_characters_entry =
        following_characters_by_token_index.insert({right, aggregator.result}).first;

      // TODO - Refactor this. In general, a keyword token cannot be followed immediately by
      // another alphanumeric character. But this requirement is currently not expressed anywhere in
      // the grammar. So without this hack, we would be overly conservative about merging parse
      // states because we would often consider `identifier` tokens to *conflict* with keyword
      // tokens.
      if (is_keyword(grammar.variables[right])) {
        following_characters_entry->second
          .exclude('a', 'z')
          .exclude('A', 'Z')
          .exclude('0', '9')
          .exclude('_')
          .exclude('$');
      }
    }

    current_conflict_detection_token_index = right;
    current_conflict_detection_following_characters = following_characters_entry->second;
    add_lex_state(item_set_for_terminals({{Symbol::terminal(left), {}}, {Symbol::terminal(right), {}}}));
    return current_conflict_value;
  }

  bool is_keyword(const LexicalVariable &variable) {
    return variable.is_string && iswalpha(get_last_character(variable.rule));
  }

  static uint32_t get_last_character(const Rule &rule) {
    return rule.match(
      [](const Seq &sequence) { return get_last_character(*sequence.right); },
      [](const rules::CharacterSet &rule) { return *rule.included_chars.begin(); },
      [](const rules::Metadata &rule) { return get_last_character(*rule.rule); },
      [](auto) { return 0; }
    );
  }

  LexStateId add_lex_state(const LexItemSet &item_set) {
    const auto &pair = lex_state_ids.find(item_set);
    if (pair == lex_state_ids.end()) {
      LexStateId state_id = lex_table.states.size();
      lex_table.states.push_back(LexState());
      lex_state_ids[item_set] = state_id;
      add_accept_token_actions(item_set, state_id);
      add_advance_actions(item_set, state_id);
      return state_id;
    } else {
      return pair->second;
    }
  }

  void clear() {
    lex_table.states.clear();
    lex_state_ids.clear();
    current_conflict_detection_following_characters = CharacterSet();
    current_conflict_value = false;
  }

 private:
  void add_advance_actions(const LexItemSet &item_set, LexStateId state_id) {
    for (const auto &pair : item_set.transitions()) {
      const CharacterSet &characters = pair.first;
      const LexItemSet::Transition &transition = pair.second;

      AdvanceAction action(-1, transition.precedence, transition.in_main_token);
      AcceptTokenAction &accept_action = lex_table.states[state_id].accept_action;
      if (accept_action.is_present()) {
        bool prefer_advancing = conflict_manager.resolve(transition.destination, action, accept_action);
        bool can_advance_for_accepted_token = false;
        for (const LexItem &item : transition.destination.entries) {
          if (item.lhs == accept_action.symbol) {
            can_advance_for_accepted_token = true;
          } else if (item.lhs.index == current_conflict_detection_token_index &&
                     !prefer_advancing && !transition.in_main_token) {
            current_conflict_value = true;
          }
        }

        if (accept_action.symbol.index == current_conflict_detection_token_index &&
            !can_advance_for_accepted_token &&
            (characters.intersects(separator_start_characters) ||
             (characters.intersects(current_conflict_detection_following_characters) &&
              grammar.variables[accept_action.symbol.index].is_string))) {
          current_conflict_value = true;
        }

        if (!prefer_advancing) continue;
      }

      action.state_index = add_lex_state(transition.destination);
      lex_table.states[state_id].advance_actions[characters] = action;
    }
  }

  void add_accept_token_actions(const LexItemSet &item_set, LexStateId state_id) {
    for (const LexItem &item : item_set.entries) {
      LexItem::CompletionStatus completion_status = item.completion_status();
      if (completion_status.is_done) {
        AcceptTokenAction action(item.lhs, completion_status.precedence.max,
                                 item.lhs.is_built_in() ||
                                 grammar.variables[item.lhs.index].is_string);
        AcceptTokenAction &existing_action = lex_table.states[state_id].accept_action;
        if (!existing_action.is_present() ||
            conflict_manager.resolve(action, existing_action)) {
          lex_table.states[state_id].accept_action = action;
        }
      }
    }
  }

  void mark_fragile_tokens(ParseTable *parse_table) {
    for (ParseState &state : parse_table->states) {
      for (auto &entry : state.terminal_entries) {
        Symbol symbol = entry.first;
        if (symbol.is_terminal()) {
          auto homonyms = conflict_manager.possible_homonyms.find(symbol.index);
          if (homonyms != conflict_manager.possible_homonyms.end())
            for (Symbol::Index homonym : homonyms->second)
              if (state.terminal_entries.count(Symbol::terminal(homonym))) {
                entry.second.reusable = false;
                break;
              }

          if (!entry.second.reusable)
            continue;

          auto extensions = conflict_manager.possible_extensions.find(symbol.index);
          if (extensions != conflict_manager.possible_extensions.end())
            for (Symbol::Index extension : extensions->second)
              if (state.terminal_entries.count(Symbol::terminal(extension))) {
                entry.second.depends_on_lookahead = true;
                break;
              }
        }
      }
    }
  }

  void remove_duplicate_lex_states(ParseTable *parse_table) {
    for (LexState &state : lex_table.states) {
      state.accept_action.is_string = false;
      state.accept_action.precedence = 0;
    }

    map<LexStateId, LexStateId> replacements;

    while (true) {
      map<LexStateId, LexStateId> duplicates;
      for (LexStateId i = 0, size = lex_table.states.size(); i < size; i++) {
        for (LexStateId j = 0; j < i; j++) {
          if (!duplicates.count(j) && lex_table.states[j] == lex_table.states[i]) {
            duplicates.insert({ i, j });
            break;
          }
        }
      }

      if (duplicates.empty()) break;

      map<LexStateId, LexStateId> new_replacements;
      for (LexStateId i = 0, size = lex_table.states.size(); i < size; i++) {
        LexStateId new_state_index = i;
        auto duplicate = duplicates.find(i);
        if (duplicate != duplicates.end()) {
          new_state_index = duplicate->second;
        }

        size_t prior_removed = 0;
        for (const auto &duplicate : duplicates) {
          if (duplicate.first >= new_state_index) break;
          prior_removed++;
        }

        new_state_index -= prior_removed;
        new_replacements.insert({i, new_state_index});
        replacements.insert({ i, new_state_index });
        for (auto &replacement : replacements) {
          if (replacement.second == i) {
            replacement.second = new_state_index;
          }
        }
      }

      for (auto &state : lex_table.states) {
        for (auto &entry : state.advance_actions) {
          auto new_replacement = new_replacements.find(entry.second.state_index);
          if (new_replacement != new_replacements.end()) {
            entry.second.state_index = new_replacement->second;
          }
        }
      }

      for (auto i = duplicates.rbegin(); i != duplicates.rend(); ++i) {
        lex_table.states.erase(lex_table.states.begin() + i->first);
      }
    }

    for (ParseState &parse_state : parse_table->states) {
      auto replacement = replacements.find(parse_state.lex_state_id);
      if (replacement != replacements.end()) {
        parse_state.lex_state_id = replacement->second;
      }
    }
  }

  LexItemSet item_set_for_terminals(const map<Symbol, ParseTableEntry> &terminals) {
    LexItemSet result;
    for (const auto &pair : terminals) {
      Symbol symbol = pair.first;
      if (symbol.is_terminal()) {
        for (const auto &rule : rules_for_symbol(symbol)) {
          for (const auto &separator_rule : separator_rules) {
            result.entries.insert(LexItem(
              symbol,
              Metadata::separator(
                Rule::seq({
                  separator_rule,
                  Metadata::main_token(rule)
                })
              )
            ));
          }
        }
      }
    }
    return result;
  }

  vector<Rule> rules_for_symbol(const rules::Symbol &symbol) {
    if (symbol == rules::END_OF_INPUT()) {
      return { CharacterSet().include(0) };
    }

    return grammar.variables[symbol.index].rule.match(
      [](const Choice &choice) {
        return choice.elements;
      },

      [](auto rule) {
        return vector<Rule>{ rule };
      }
    );
  }
};

unique_ptr<LexTableBuilder> LexTableBuilder::create(const LexicalGrammar &grammar) {
  return unique_ptr<LexTableBuilder>(new LexTableBuilderImpl(grammar));
}

LexTable LexTableBuilder::build(ParseTable *parse_table) {
  return static_cast<LexTableBuilderImpl *>(this)->build(parse_table);
}

bool LexTableBuilder::detect_conflict(Symbol::Index left, Symbol::Index right,
                                      const vector<set<Symbol::Index>> &following_terminals) {
  return static_cast<LexTableBuilderImpl *>(this)->detect_conflict(left, right, following_terminals);
}

}  // namespace build_tables
}  // namespace tree_sitter
