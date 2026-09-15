#include "patchouli/config.hpp"
#include "patchouli/patchouli.hpp"
#include "patchouli/json_io.hpp"
#include "patchouli/ngram.hpp"
#include "patchouli/pta.hpp"
#include "patchouli/rsr.hpp"
#include "patchouli/state_merge.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <limits>
#include <thread>

using namespace patchouli;
class SetOracle final : public Oracle {
public:
    explicit SetOracle(std::set<std::string> accepted) : accepted_(std::move(accepted)) {}
    std::vector<bool> accepts_batch(const std::vector<std::string>& values) override { batches.push_back(values); std::vector<bool> result; for (const auto& text:values) { calls.push_back(text); result.push_back(accepted_.contains(text)); } return result; }
    std::vector<std::vector<std::string>> batches;
    std::vector<std::string> calls;
private:
    std::set<std::string> accepted_;
};
int main() {
    // Wall timing must include waiting and accumulate both initial and repair calls.
    class WaitingOracle final : public Oracle {
    public:
        unsigned calls{};
        std::vector<std::string> last_batch;
        std::vector<bool> accepts_batch(const std::vector<std::string>& values) override {
            last_batch=values;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return std::vector<bool>(values.size(), ++calls>=2);
        }
    };
    // Retention must be a prefix of the complete, score-sorted candidate list.
    const auto unlimited=std::numeric_limits<std::size_t>::max();
    const std::vector<std::string> batch_words{"aa","ab","ac","ad","ae","af","ag","ah","ai","aj"};
    auto batch_pta=build_pta(batch_words,100);
    for (std::size_t n=0;n<=5;++n) {
        NGramModel batch_model({"aj","aj","ai","ab"},n);
        auto all=rsr_repairs(batch_pta,"az",batch_model,unlimited,unlimited,unlimited,unlimited);
        assert(all.size()==batch_words.size());
        for (std::size_t i=1;i<all.size();++i) {
            assert(all[i-1].ngram_score>=all[i].ngram_score);
            if (all[i-1].ngram_score==all[i].ngram_score)
                assert(all[i-1].value<all[i].value);
        }
        for (std::size_t batch:{1u,2u,4u,8u,16u}) {
            auto top=rsr_repairs(batch_pta,"az",batch_model,batch,unlimited,unlimited,unlimited);
            assert(top.size()==std::min(batch,all.size()));
            for (std::size_t i=0;i<top.size();++i) {
                assert(top[i].value==all[i].value);
                assert(top[i].ngram_score==all[i].ngram_score);
            }
        }
        auto small=build_pta({"aa","ab"},100);
        auto retained=rsr_repairs(small,"az",batch_model,8,unlimited,unlimited,unlimited);
        assert(retained.size()==2);
    }
    auto pta=build_pta({"", "ab", "ac"},100);
    assert(pta.accepts("")); assert(pta.accepts("ab")); assert(!pta.accepts("a"));
    StateId a{}; assert(pta.transition(pta.start_state(),'a',a));
    StateId b{},c{}; assert(pta.transition(a,'b',b)); assert(pta.transition(a,'c',c));

    // Closure can combine different continuation languages at any depth.
    auto folding_pta=build_pta({"a0x","b0y"},100);
    StateId qa{},qb{};
    assert(folding_pta.transition(folding_pta.start_state(),'a',qa));
    assert(folding_pta.transition(folding_pta.start_state(),'b',qb));
    PartitionedDfa folding(folding_pta);
    const auto folding_before=folding.materialize(folding_pta).fingerprint();
    const auto folding_checkpoint=folding.checkpoint();
    folding.merge_with_closure(qa,qb);
    for (const std::string value:{"a0x","b0y","a0y","b0x"})
        assert(folding.accepts(folding_pta,value));
    assert(!rejects_all(folding_pta,folding,{"a0y"}));
    folding.rollback(folding_checkpoint);
    assert(folding.materialize(folding_pta).fingerprint()==folding_before);
    NGramModel model({"aaaa","aaab"},2); assert(model.score("aaaa")>model.score("zzzz"));
    NGramModel disabled_model({"a"},0,"z");
    assert(disabled_model.score("anything")==0.0);

    // Start/end markers must not collide with any of the 256 byte symbols.
    const std::string control_bytes{"\x02\x03",2};
    NGramModel byte_model({control_bytes},2);
    assert(byte_model.score(control_bytes)>byte_model.score(std::string{"\x02\x02",2}));

    // The corrupt input contributes symbols to V but not observations.
    NGramModel training_vocabulary({"a"},1);
    NGramModel expanded_vocabulary({"a"},1,"z");
    assert(expanded_vocabulary.score("a")<training_vocabulary.score("a"));
    auto repair=rsr_repair(pta,"ad"); assert(repair);
    assert(repair->edit_distance==1); assert(repair->value=="ab" || repair->value=="ac");

    // Exact exclusion rejects only selected word. Prefixes, extensions, and
    // strings diverging from selected path preserve source behavior.
    auto exclusion_source=build_pta({"","a","ab","abc","ac","b"},100);
    auto without_ab=reject_exact_string(exclusion_source,"ab");
    for (const std::string value:{"","a","abc","ac","b"})
        assert(without_ab.accepts(value)==exclusion_source.accepts(value));
    assert(exclusion_source.accepts("ab")); assert(!without_ab.accepts("ab"));
    std::vector<std::string> exhaustive{""};
    for (std::size_t length=0;length<4;++length) {
        const auto prefixes=exhaustive;
        for (const auto& prefix:prefixes) {
            if (prefix.size()!=length) continue;
            for (char symbol:std::string{"abc"}) exhaustive.push_back(prefix+symbol);
        }
    }
    for (const auto& value:exhaustive)
        assert(without_ab.accepts(value)==
               (exclusion_source.accepts(value) && value!="ab"));
    auto without_ab_ac=reject_exact_string(without_ab,"ac");
    for (const auto& value:exhaustive)
        assert(without_ab_ac.accepts(value)==
               (exclusion_source.accepts(value) && value!="ab" && value!="ac"));
    auto without_empty=reject_exact_string(exclusion_source,"");
    assert(!without_empty.accepts("")); assert(without_empty.accepts("a"));
    auto unchanged=reject_exact_string(exclusion_source,"missing");
    for (const std::string value:{"","a","ab","abc","ac","b","missing"})
        assert(unchanged.accepts(value)==exclusion_source.accepts(value));
    auto in_place_exclusion=exclusion_source;
    assert(reject_exact_string_in_place(in_place_exclusion,"ab"));
    assert(!reject_exact_string_in_place(in_place_exclusion,"ab"));
    for (const auto& value:exhaustive)
        assert(in_place_exclusion.accepts(value)==
               (exclusion_source.accepts(value) && value!="ab"));
    RsrIterationMeasurement minimum_measurement;
    auto minimum_repairs=rsr_minimum_repairs(
        pta,"ad",std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),&minimum_measurement);
    assert(minimum_repairs.size()==2);
    assert(minimum_repairs[0].value=="ab");
    assert(minimum_repairs[1].value=="ac");
    assert(minimum_repairs[0].edit_distance==1 && minimum_repairs[1].edit_distance==1);
    assert(minimum_measurement.minimum_edit_cost==1);
    assert(minimum_measurement.unique_candidates==2);
    assert(minimum_measurement.enumeration_complete);
    auto capped_repairs=rsr_minimum_repairs(pta,"ad",20,1);
    assert(capped_repairs.size()==1);
    auto repairs=rsr_repairs(pta,"ad",NGramModel({"ab"},2),1,20,100,10000);
    assert(repairs.size()==1);
    assert(repairs[0].value=="ab");
    assert(repairs[0].edit_distance==1);
    auto tiered=rsr_repairs(pta,"ad",NGramModel({"ab"},2),
        std::numeric_limits<std::size_t>::max(),20,100,10000);
    assert(tiered.size()==2);
    std::set<std::string> tiered_values;
    for (const auto& candidate:tiered) tiered_values.insert(candidate.value);
    assert(tiered_values==std::set<std::string>({"ab","ac"}));
    auto unseen=rsr_repairs(pta,"ad",NGramModel({"ab"},2),1,20,100,10000,{"ab"});
    assert(unseen.size()==1); assert(unseen.front().value=="ac");

    // Paper example: b(ab)* repaired from "bba" has minimum distance 2.
    Automaton cyclic;
    auto q0=cyclic.add_state(false), q1=cyclic.add_state(true),
         q2=cyclic.add_state(false);
    cyclic.set_start_state(q0);
    cyclic.state(q0).transitions.emplace('b',q1);
    cyclic.state(q1).transitions.emplace('a',q2);
    cyclic.state(q2).transitions.emplace('b',q1);
    auto paper_repair=rsr_repair(cyclic,"bba");
    assert(paper_repair);
    assert(paper_repair->edit_distance==2);
    assert(cyclic.accepts(paper_repair->value));
    auto merged=state_merge(pta,{"aa"});
    assert(!merged.partition.materialize(pta).accepts("aa"));
    // Learning keeps the PTA immutable and stores selected merges in a
    // lightweight partition. Materialization must preserve the quotient
    // language without modifying the base PTA.
    assert(pta.accepts("ab")); assert(!pta.accepts("aa"));
    auto rematerialized=merged.partition.materialize(pta);
    assert(rematerialized.accepts("ab")); assert(!rematerialized.accepts("aa"));

    // Speculative quotient merges must be exactly reversible, including
    // transitions, class metadata, and the constant-time evidence counter.
    PartitionedDfa transactional(pta);
    const auto original_fingerprint=transactional.materialize(pta).fingerprint();
    const auto original_accepting=transactional.accepting_class_count();
    const auto checkpoint=transactional.checkpoint();
    transactional.merge_with_closure(b,c);
    assert(transactional.accepting_class_count()==original_accepting-1);
    transactional.rollback(checkpoint);
    assert(transactional.accepting_class_count()==original_accepting);
    assert(transactional.materialize(pta).fingerprint()==original_fingerprint);

    // EDSM ranks label agreement above incidental structural compression.
    // Merging q0-qA folds qX-qY too (two removed states), but joins no two
    // accepting classes. Merging q0-qB removes only qB, but has evidence 1.
    Automaton evidence_pta;
    auto e0=evidence_pta.add_state(true), eA=evidence_pta.add_state(false),
         eB=evidence_pta.add_state(true), eX=evidence_pta.add_state(false),
         eY=evidence_pta.add_state(false);
    evidence_pta.set_start_state(e0);
    evidence_pta.state(e0).transitions.emplace('a',eA);
    evidence_pta.state(e0).transitions.emplace('b',eB);
    evidence_pta.state(e0).transitions.emplace('x',eX);
    evidence_pta.state(eA).transitions.emplace('x',eY);
    PartitionedDfa structural_candidate(evidence_pta), labelled_candidate(evidence_pta);
    const auto accepting_before=structural_candidate.accepting_class_count();
    structural_candidate.merge_with_closure(e0,eA);
    labelled_candidate.merge_with_closure(e0,eB);
    assert(accepting_before-structural_candidate.accepting_class_count()==0);
    assert(accepting_before-labelled_candidate.accepting_class_count()==1);
    auto evidence_merge=state_merge(evidence_pta,{});
    assert(!evidence_merge.history.empty());
    assert(evidence_merge.history.front().blue_original_states==std::vector<StateId>{eB});

    // Equal evidence chooses the lowest blue ID, even when its acceptance
    // differs from red. With no valid merge, promotion preserves the PTA.
    auto tie_pta=build_pta({"a","b"},100);
    auto tie_merge=state_merge(tie_pta,{});
    assert(tie_merge.history.front().red_original_states==std::vector<StateId>{0});
    assert(tie_merge.history.front().blue_original_states==std::vector<StateId>{1});
    auto red_tie_pta=build_pta({"ax"},100);
    auto red_tie=state_merge(red_tie_pta,{"x"});
    assert(red_tie.history.front().red_original_states==std::vector<StateId>{0});
    assert(red_tie.history.front().blue_original_states==std::vector<StateId>{2});
    auto promoted_pta=build_pta({"a"},100);
    auto promoted=state_merge(promoted_pta,{""});
    assert(promoted.history.empty());
    assert(promoted.partition.accepts(promoted_pta,"a"));
    assert(!promoted.partition.accepts(promoted_pta,""));

    // Evidence includes accepting classes folded below nonaccepting endpoints.
    auto closure_pta=build_pta({"ax","bx"},100);
    PartitionedDfa closure(closure_pta);
    const auto closure_accepting=closure.accepting_class_count();
    StateId ca{},cb{};
    assert(closure_pta.transition(0,'a',ca));
    assert(closure_pta.transition(0,'b',cb));
    closure.merge_with_closure(ca,cb);
    assert(closure_accepting-closure.accepting_class_count()==1);

    // Replay commits a valid prefix, then rolls back a later merge that
    // admits a newly learned negative. Endpoint identities remain exact.
    const MergeHistory replay_history{{{1},{2}},{{0},{1,2}}};
    auto valid_replay=replay_merges(tie_pta,{},replay_history);
    assert(valid_replay.valid_history.size()==2);
    assert(valid_replay.partition.accepts(tie_pta,""));
    auto conflicted_replay=replay_merges(tie_pta,{""},replay_history);
    assert(conflicted_replay.valid_history.size()==1);
    assert(!conflicted_replay.partition.accepts(tie_pta,""));
    assert(conflicted_replay.partition.accepts(tie_pta,"a"));
    assert(conflicted_replay.partition.accepts(tie_pta,"b"));
    PartitionedDfa expected_prefix(tie_pta);
    expected_prefix.merge_with_closure(1,2);
    assert(conflicted_replay.partition.materialize(tie_pta).fingerprint()==
           expected_prefix.materialize(tie_pta).fingerprint());
    auto unavailable_replay=replay_merges(tie_pta,{},MergeHistory{{{0,1},{2}}});
    assert(unavailable_replay.valid_history.empty());

    auto input=parse_input_value(nlohmann::json{{"positive_examples",{"x"}},{"corrupt_string","y"}});
    assert(input.negative_examples.empty());
    auto config=parse_config_value(nlohmann::json{
        {"oracle",{{"executable","oracle"}}},
        {"repair",{{"n",2},{"ngrams_batch_size",-1},{"max_candidate_length",-1}}},
        {"limits",{{"max_iterations",8},{"max_states",100},{"max_queue_size",-1},{"max_rsr_candidates",-1}}}});
    assert(!config.seed);
    nlohmann::json minimal_config{
        {"oracle",{{"executable","oracle"}}},
        {"repair",{{"n",0},{"ngrams_batch_size",1}}},
        {"limits",{{"max_iterations",1},{"max_states",10}}}};
    assert(parse_config_value(minimal_config).n==0);
    minimal_config["state_merging"]=nlohmann::json::object();
    assert(parse_config_value(minimal_config).n==0);
    for (const auto& obsolete_value : {nlohmann::json(0),nlohmann::json(3),nlohmann::json(nullptr)}) {
        minimal_config["state_merging"]["k"]=obsolete_value;
        bool rejected_k=false;
        try { (void)parse_config_value(minimal_config); }
        catch (const std::runtime_error& error) {
            rejected_k=std::string(error.what()).find("state_merging.k is no longer supported; remove it")!=std::string::npos;
        }
        assert(rejected_k);
    }
    assert(config.ngrams_batch_size==std::numeric_limits<std::size_t>::max());
    assert(config.max_candidate_length==std::numeric_limits<std::size_t>::max());
    assert(config.max_queue_size==std::numeric_limits<std::size_t>::max());
    assert(config.max_rsr_candidates==std::numeric_limits<std::size_t>::max());
    bool rejected_rsr_batch=false;
    try {
        parse_config_value(nlohmann::json{
            {"oracle",{{"executable","oracle"}}},
            {"repair",{{"n",2},{"rsr_batch_size",2},{"ngrams_batch_size",1}}},
            {"limits",{{"max_iterations",1},{"max_states",10}}}});
    } catch (const std::runtime_error&) { rejected_rsr_batch=true; }
    assert(rejected_rsr_batch);
    SetOracle oracle({"ab"});
    AlgorithmMeasurements measurements;
    assert(patchouli::patchouli(InputData{{"ab"},{},"ac"},config,oracle,&measurements)=="ab");
    assert(oracle.calls.front()=="ac");
    assert(measurements.total_iterations>=1);
    WaitingOracle waiting;
    AlgorithmMeasurements waiting_measurements;
    const auto first_accepted=patchouli::patchouli(InputData{{"ab"},{},"ac"},config,waiting,
                               &waiting_measurements);
    assert(first_accepted==waiting.last_batch.front());
    assert(first_accepted!="ac");
    assert(waiting.calls==2);
    assert(waiting_measurements.oracle_total_calls==2);
    assert(waiting_measurements.oracle_execution_time_ns>=20000000);
    WaitingOracle initial_accept;
    initial_accept.calls=1;
    AlgorithmMeasurements initial_measurements;
    assert(patchouli::patchouli(InputData{{"ab"},{},"ac"},config,initial_accept,
                               &initial_measurements)=="ac");
    assert(initial_measurements.oracle_total_calls==1);
    assert(initial_measurements.oracle_execution_time_ns>=10000000);
    assert(initial_measurements.total_iterations==0);
    assert(measurements.edsm_execution_time_ns==
           measurements.initial_state_merge_ns+
           measurements.merge_replay_ns+
           measurements.resumed_state_merge_ns);
    auto measured_json=nlohmann::json::parse(serialize_result(
        ProgramResult{"ab",0,0,1,measurements}));
    assert(measured_json.at("total_iterations")==measurements.total_iterations);
    assert(measured_json.contains("rsr_execution_time_ns"));
    assert(measured_json.at("oracle_total_calls")==measurements.oracle_total_calls);
    assert(measured_json.at("oracle_execution_time_ns")==measurements.oracle_execution_time_ns);
    assert(!measured_json.contains("ktails_execution_time_ns"));
    assert(measured_json.contains("initial_state_merge_ns"));
    assert(measured_json.contains("merge_replay_ns"));
    assert(measured_json.contains("resumed_state_merge_ns"));
    assert(measured_json.contains("candidate_copy_or_rollback_ns"));
    assert(measured_json.contains("negative_validation_ns"));
    assert(measured_json.at("rsr_total_calls")==measurements.rsr_iterations.size());
    assert(measured_json.at("rsr_candidates_generated").get<std::size_t>()>=1);
    assert(measured_json.at("rsr_max_candidates_in_call").get<std::size_t>()>=1);
    assert(measured_json.contains("rsr_calls_with_multiple_candidates"));
    assert(!measured_json.at("rsr_enumeration_truncated").get<bool>());
    assert(measured_json.at("rsr_iterations").is_array());
    SetOracle rejecting({});
    bool rejected_batch_failed=false;
    try { (void)patchouli::patchouli(InputData{{"ab","ac"},{},"ad"},config,rejecting); }
    catch (const std::runtime_error&) { rejected_batch_failed=true; }
    assert(rejected_batch_failed); assert(rejecting.calls.size()>=3);
    assert(rejecting.calls[0]=="ad");
    assert(rejecting.calls[1]!=rejecting.calls[2]);
    assert(rejecting.batches.front()==std::vector<std::string>{"ad"});
    assert(rejecting.batches[1].size()>=2);
    std::set<std::string> queried;
    for (const auto& batch:rejecting.batches)
        for (const auto& value:batch) assert(queried.insert(value).second);
    SetOracle known_negative({"ab"});
    assert(patchouli::patchouli(InputData{{"ab"},{"aa"},"ac"},config,known_negative)=="ab");
    assert(std::find(known_negative.calls.begin(),known_negative.calls.end(),"aa")==known_negative.calls.end());
    auto replay=replay_merges(pta,{"aa"},merged.history);
    assert(replay.valid_history.size()==merged.history.size());
    assert(replay.partition.materialize(pta).fingerprint()==rematerialized.fingerprint());
    std::cout << "ok\n";
}
