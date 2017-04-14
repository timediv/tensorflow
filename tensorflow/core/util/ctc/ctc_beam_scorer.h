/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Collection of scoring classes that can be extended and provided to the
// CTCBeamSearchDecoder to incorporate additional scoring logic (such as a
// language model).
//
// To build a custom scorer extend and implement the pure virtual methods from
// BeamScorerInterface. The default CTC decoding behavior is implemented
// through BaseBeamScorer.

#ifndef TENSORFLOW_CORE_UTIL_CTC_CTC_BEAM_SCORER_H_
#define TENSORFLOW_CORE_UTIL_CTC_CTC_BEAM_SCORER_H_

#include "tensorflow/core/util/ctc/ctc_beam_entry.h"
#include "lm/model.hh"

#include <iostream>
#include <fstream>

namespace tensorflow {
namespace ctc {

using namespace ctc_beam_search;

// Base implementation of a beam scorer used by default by the decoder that can
// be subclassed and provided as an argument to CTCBeamSearchDecoder, if complex
// scoring is required. Its main purpose is to provide a thin layer for
// integrating language model scoring easily.
template <typename CTCBeamState>
class BaseBeamScorer {
 public:
  virtual ~BaseBeamScorer() {}
  // State initialization.
  virtual void InitializeState(CTCBeamState* root) const {}
  // ExpandState is called when expanding a beam to one of its children.
  // Called at most once per child beam. In the simplest case, no state
  // expansion is done.
  virtual void ExpandState(const CTCBeamState& from_state, int from_label,
                           CTCBeamState* to_state, int to_label) const {}
  // ExpandStateEnd is called after decoding has finished. Its purpose is to
  // allow a final scoring of the beam in its current state, before resorting
  // and retrieving the TopN requested candidates. Called at most once per beam.
  virtual void ExpandStateEnd(CTCBeamState* state) const {}
  // GetStateExpansionScore should be an inexpensive method to retrieve the
  // (cached) expansion score computed within ExpandState. The score is
  // multiplied (log-addition) with the input score at the current step from
  // the network.
  //
  // The score returned should be a log-probability. In the simplest case, as
  // there's no state expansion logic, the expansion score is zero.
  virtual float GetStateExpansionScore(const CTCBeamState& state,
                                       float previous_score) const {
    return previous_score;
  }
  // GetStateEndExpansionScore should be an inexpensive method to retrieve the
  // (cached) expansion score computed within ExpandStateEnd. The score is
  // multiplied (log-addition) with the final probability of the beam.
  //
  // The score returned should be a log-probability.
  virtual float GetStateEndExpansionScore(const CTCBeamState& state) const {
    return 0;
  }
};

class LabelToCharacterTranslator {
 public:
  LabelToCharacterTranslator() {}

  bool inline IsBlankLabel(int label) const {
    return label == 28;
  }

  bool inline IsSpaceLabel(int label) const {
    return label == 27;
  }

  char GetCharacterFromLabel(int label) const {
    if (label == 26) {
      return '\'';
    }
    if (label == 27) {
      return ' ';
    }
    return label + 'a';
  }
};

class KenLMBeamScorer : public BaseBeamScorer<KenLMBeamState> {
 public:
  typedef lm::ngram::ProbingModel Model;

  virtual ~KenLMBeamScorer() {
    delete model;
    delete trieRoot;
  }
  KenLMBeamScorer(const char *kenlm_file_path) {
    lm::ngram::Config config;
    config.load_method = util::POPULATE_OR_READ;
    model = new Model(kenlm_file_path, config);

    std::string trie_path = std::string(kenlm_file_path) + ".trie";
    std::ifstream in;
    in.open(trie_path.c_str(), std::ios::in);
    in >> trieRoot;
    in.close();
  }

  // State initialization.
  void InitializeState(KenLMBeamState* root) const {
    root->language_model_score = 0.0f;
    root->score = 0.0f;
    root->delta_score = 0.0f;
    root->incomplete_word.clear();
    root->incomplete_word_trie_node = trieRoot;
    root->model_state = model->BeginSentenceState();
  }
  // ExpandState is called when expanding a beam to one of its children.
  // Called at most once per child beam. In the simplest case, no state
  // expansion is done.
  void ExpandState(const KenLMBeamState& from_state, int from_label,
                           KenLMBeamState* to_state, int to_label) const {
    CopyState(from_state, to_state);

    if (from_label == to_label || translator.IsBlankLabel(to_label)) {
      to_state->delta_score = 0.0f;
      return;
    }

    if (!translator.IsSpaceLabel(to_label)) {
      to_state->incomplete_word += translator.GetCharacterFromLabel(to_label);
      TrieNode<27> *trie_node = from_state.incomplete_word_trie_node;

      float prefix_prob = -10.0f;
      // If prefix does exist
      if (trie_node != nullptr) {
        trie_node = trie_node->GetChildAt(to_label);
        to_state->incomplete_word_trie_node = trie_node;

        if (trie_node != nullptr) {
          prefix_prob = static_cast<float>(trie_node->GetFrequency()) /
                  static_cast<float>(trieRoot->GetFrequency());
          // TODO store and retrieve the least likely extension
          // Convert to log probability
          prefix_prob = std::log10(prefix_prob);
        }
      }
      // TODO try two options
      // 1) unigram score added up to language model scare
      // 2) langugage model score of (preceding_words + unigram_word)
      to_state->score = prefix_prob + to_state->language_model_score;
      to_state->delta_score = to_state->score - from_state.score;

    } else {
      float probability = ScoreIncompleteWord(from_state.model_state,
                            to_state->incomplete_word,
                            to_state->model_state);
      UpdateWithLMScore(to_state, probability);
      ResetIncompleteWord();
    }
  }
  // ExpandStateEnd is called after decoding has finished. Its purpose is to
  // allow a final scoring of the beam in its current state, before resorting
  // and retrieving the TopN requested candidates. Called at most once per beam.
  void ExpandStateEnd(KenLMBeamState* state) const {
    Model::State out;
    lm::FullScoreReturn full_score_return;
    if (state->incomplete_word.size() > 0) {
      ScoreIncompleteWord(state->model_state, state->incomplete_word, out);
      ResetIncompleteWord(state);
      state->model_state = out;
    }
    full_score_return = model->FullScore(state->model_state,
                            model->GetVocabulary().EndSentence(),
                            out);
    UpdateWithLMScore(state, full_score_return.prob);
  }
  // GetStateExpansionScore should be an inexpensive method to retrieve the
  // (cached) expansion score computed within ExpandState. The score is
  // multiplied (log-addition) with the input score at the current step from
  // the network.
  //
  // The score returned should be a log-probability. In the simplest case, as
  // there's no state expansion logic, the expansion score is zero.
  float GetStateExpansionScore(const KenLMBeamState& state,
                                       float previous_score) const {
    return state.delta_score + previous_score;
  }
  // GetStateEndExpansionScore should be an inexpensive method to retrieve the
  // (cached) expansion score computed within ExpandStateEnd. The score is
  // multiplied (log-addition) with the final probability of the beam.
  //
  // The score returned should be a log-probability.
  float GetStateEndExpansionScore(const KenLMBeamState& state) const {
    return state.delta_score;
  }

 private:
  LabelToCharacterTranslator translator;
  TrieNode<27> *trieRoot;
  Model *model;

  void UpdateWithLMScore(KenLMBeamState *state, float lm_score) {
    float previous_score = state->score;
    state->language_model_score = lm_score;
    state->score = lm_score;
    state->delta_score = lm_score - previous_score;
  }

  void ResetIncompleteWord(KenLMBeamState *state) {
    state->incomplete_word.clear();
    state->incomplete_word_trie_node = trieRoot;
  }

  float ScoreIncompleteWord(const Model::State& model_state,
                            const std::string& word,
                            Model::State& out) const {
    lm::FullScoreReturn full_score_return;
    lm::WordIndex vocab;
    vocab = model->GetVocabulary().Index(word);
    full_score_return = model->FullScore(model_state, vocab, out);
    return full_score_return.prob;
  }

  void CopyState(const KenLMBeamState& from, KenLMBeamState* to) const {
    to->language_model_score = from.language_model_score;
    to->score = from.score;
    to->delta_score = from.delta_score;
    to->incomplete_word = from.incomplete_word;
    to->incomplete_word_trie_node = from.incomplete_word_trie_node;
    to->model_state = from.model_state;
  }

};

class PrefixScorer : public BaseBeamScorer<PrefixBeamState> {
 public:

  virtual ~PrefixScorer() {
    delete trieRoot;
  }

  PrefixScorer(const char *trie_path) {
    std::ifstream in;
    in.open(trie_path, std::ios::in);
    in >> trieRoot;
    in.close();
  }

  // State initialization.
  void InitializeState(PrefixBeamState* root) const {
    root->prob = 0;
    root->node = trieRoot;
  }
  // ExpandState is called when expanding a beam to one of its children.
  // Called at most once per child beam. In the simplest case, no state
  // expansion is done.
  void ExpandState(const PrefixBeamState& from_state, int from_label,
                           PrefixBeamState* to_state, int to_label) const {
    CopyState(from_state, to_state);

    if (from_label == to_label || translator.IsBlankLabel(to_label)) {
      return;
    }

    if (translator.IsSpaceLabel(to_label)) {
      to_state->node = trieRoot;
      return;
    }

    if (to_state->node == nullptr) {
      // We already figured out that no prefix exists
      // Penalty already applied (only once per word)
      return;
    }

    to_state->node = to_state->node->GetChildAt(to_label);
    if (to_state->node == nullptr) {
      // Add penalty for non existing prefix
      to_state->prob -= 1.0f;
    }
  }
  // ExpandStateEnd is called after decoding has finished. Its purpose is to
  // allow a final scoring of the beam in its current state, before resorting
  // and retrieving the TopN requested candidates. Called at most once per beam.
  void ExpandStateEnd(PrefixBeamState* state) const {}
  // GetStateExpansionScore should be an inexpensive method to retrieve the
  // (cached) expansion score computed within ExpandState. The score is
  // multiplied (log-addition) with the input score at the current step from
  // the network.
  //
  // The score returned should be a log-probability. In the simplest case, as
  // there's no state expansion logic, the expansion score is zero.
  float GetStateExpansionScore(const PrefixBeamState& state,
                                       float previous_score) const {
    return state.prob;
  }
  // GetStateEndExpansionScore should be an inexpensive method to retrieve the
  // (cached) expansion score computed within ExpandStateEnd. The score is
  // multiplied (log-addition) with the final probability of the beam.
  //
  // The score returned should be a log-probability.
  float GetStateEndExpansionScore(const PrefixBeamState& state) const {
    return state.prob;
  }

 private:
  TrieNode<27> *trieRoot;
  LabelToCharacterTranslator translator;

  void CopyState(const PrefixBeamState& from, PrefixBeamState* to) const {
    to->prob = from.prob;
    to->node = from.node;
  }
};

}  // namespace ctc
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_UTIL_CTC_CTC_BEAM_SCORER_H_
