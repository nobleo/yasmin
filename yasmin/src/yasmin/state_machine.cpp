// Copyright (C) 2023  Miguel Ángel González Santamarta

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "yasmin/blackboard/blackboard.hpp"
#include "yasmin/logs.hpp"
#include "yasmin/state.hpp"
#include "yasmin/state_machine.hpp"

using namespace yasmin;

StateMachine::StateMachine(std::vector<std::string> outcomes)
    : State(outcomes) {
  this->current_state_mutex = std::make_unique<std::mutex>();
}

void StateMachine::add_state(std::string name, std::shared_ptr<State> state,
                             std::map<std::string, std::string> transitions) {

  if (this->states.find(name) != this->states.end()) {
    throw std::logic_error("State '" + name +
                           "' already registered in the state machine");
  }

  for (auto it = transitions.begin(); it != transitions.end(); ++it) {
    const std::string &key = it->first;
    const std::string &value = it->second;

    if (key.empty()) {
      throw std::invalid_argument("Transitions with empty source in state '" +
                                  name + "'");
    }

    if (value.empty()) {
      throw std::invalid_argument("Transitions with empty target in state '" +
                                  name + "'");
    }

    if (std::find(state->get_outcomes().begin(), state->get_outcomes().end(),
                  key) == state->get_outcomes().end()) {
      std::ostringstream oss;
      oss << "State '" << name << "' references unregistered outcomes '" << key
          << "', available outcomes are ";
      for (const auto &outcome : state->get_outcomes()) {
        oss << outcome << " ";
      }
      throw std::invalid_argument(oss.str());
    }
  }

  this->states.insert({name, state});
  this->transitions.insert({name, transitions});

  if (this->start_state.empty()) {
    this->start_state = name;
  }
}

void StateMachine::add_state(std::string name, std::shared_ptr<State> state) {
  this->add_state(name, state, {});
}

void StateMachine::set_start_state(std::string state_name) {

  if (state_name.empty()) {
    throw std::invalid_argument("Initial state cannot be empty");

  } else if (this->states.find(state_name) == this->states.end()) {
    throw std::invalid_argument("Initial state '" + state_name +
                                "' is not in the state machine");
  }

  this->start_state = state_name;
}

std::string StateMachine::get_start_state() { return this->start_state; }

void StateMachine::cancel_state() {
  State::cancel_state();

  const std::lock_guard<std::mutex> lock(*this->current_state_mutex.get());
  if (!this->current_state.empty()) {
    this->states.at(this->current_state)->cancel_state();
  }
}

std::map<std::string, std::shared_ptr<State>> const &
StateMachine::get_states() {
  return this->states;
}

std::map<std::string, std::map<std::string, std::string>> const &
StateMachine::get_transitions() {
  return this->transitions;
}

std::string StateMachine::get_current_state() {
  const std::lock_guard<std::mutex> lock(*this->current_state_mutex.get());
  return this->current_state;
}

void StateMachine::validate() {

  // check initial state
  if (this->start_state.empty()) {
    throw std::runtime_error("No initial state set");
  }

  std::set<std::string> terminal_outcomes;

  // check all states
  for (auto it = this->states.begin(); it != this->states.end(); ++it) {

    const std::string &state_name = it->first;
    const std::shared_ptr<State> &state = it->second;
    std::map<std::string, std::string> transitions =
        this->transitions.at(state_name);

    std::vector<std::string> outcomes = state->get_outcomes();

    // check if all state outcomes are in transitions
    for (const std::string &o : outcomes) {

      if (transitions.find(o) == transitions.end() &&
          std::find(this->get_outcomes().begin(), this->get_outcomes().end(),
                    o) == this->get_outcomes().end()) {

        throw std::runtime_error("State '" + state_name + "' outcome '" + o +
                                 "' not registered in transitions");

      } else if (std::find(this->get_outcomes().begin(),
                           this->get_outcomes().end(),
                           o) != this->get_outcomes().end()) {
        terminal_outcomes.insert(o);
      }
    }

    // if state is a state machine, validate it
    if (std::dynamic_pointer_cast<StateMachine>(state)) {
      std::dynamic_pointer_cast<StateMachine>(state)->validate();
    }

    // add terminal outcomes
    for (auto it = transitions.begin(); it != transitions.end(); ++it) {
      const std::string &value = it->second;
      terminal_outcomes.insert(value);
    }
  }

  // check terminal outcomes for the state machine
  std::set<std::string> sm_outcomes(this->get_outcomes().begin(),
                                    this->get_outcomes().end());

  // check if all state machine outcomes are in the terminal outcomes
  for (const std::string &o : this->get_outcomes()) {
    if (terminal_outcomes.find(o) == terminal_outcomes.end()) {
      throw std::runtime_error("Target outcome '" + o +
                               "' not registered in transitions");
    }
  }

  // check if all terminal outcomes are states or state machine outcomes
  for (const std::string &o : terminal_outcomes) {
    if (this->states.find(o) == this->states.end() &&
        sm_outcomes.find(o) == sm_outcomes.end()) {
      throw std::runtime_error("State machine outcome '" + o +
                               "' not registered as outcome or state");
    }
  }
}

std::string
StateMachine::execute(std::shared_ptr<blackboard::Blackboard> blackboard) {

  this->validate();

  this->current_state_mutex->lock();
  this->current_state = this->start_state;
  this->current_state_mutex->unlock();

  std::map<std::string, std::string> transitions;
  std::string outcome;

  while (true) {

    this->current_state_mutex->lock();

    auto state = this->states.at(this->current_state);
    transitions = this->transitions.at(this->current_state);
    this->current_state_mutex->unlock();

    outcome = (*state.get())(blackboard);

    // check outcome belongs to state
    if (std::find(state->get_outcomes().begin(), state->get_outcomes().end(),
                  outcome) == this->outcomes.end()) {
      throw std::logic_error("Outcome (" + outcome +
                             ") is not register in state " +
                             this->current_state);
    }

    // translate outcome using transitions
    if (transitions.find(outcome) != transitions.end()) {

      YASMIN_LOG_INFO("%s: %s --> %s", this->current_state.c_str(),
                      outcome.c_str(), transitions.at(outcome).c_str());

      outcome = transitions.at(outcome);
    }

    // outcome is an outcome of the sm
    if (std::find(this->outcomes.begin(), this->outcomes.end(), outcome) !=
        this->outcomes.end()) {

      this->current_state_mutex->lock();
      this->current_state.clear();
      this->current_state_mutex->unlock();

      return outcome;

      // outcome is a state
    } else if (this->states.find(outcome) != this->states.end()) {

      this->current_state_mutex->lock();
      this->current_state = outcome;
      this->current_state_mutex->unlock();

      // outcome is not in the sm
    } else {
      throw std::logic_error("Outcome (" + outcome + ") without transition");
    }
  }

  return "";
}

std::string StateMachine::execute() {

  std::shared_ptr<blackboard::Blackboard> blackboard =
      std::make_shared<blackboard::Blackboard>();

  std::string outcome = this->operator()(blackboard);
  return outcome;
}

std::string StateMachine::operator()() {
  std::shared_ptr<blackboard::Blackboard> blackboard =
      std::make_shared<blackboard::Blackboard>();

  return this->operator()(blackboard);
}

std::string StateMachine::to_string() {

  std::string result = "State Machine\n";

  for (const auto &s : this->get_states()) {
    result += s.first + " (" + s.second->to_string() + ")\n";
    for (const auto &t : this->transitions.at(s.first)) {
      result += "\t" + t.first + " --> " + t.second + "\n";
    }
  }

  return result;
}