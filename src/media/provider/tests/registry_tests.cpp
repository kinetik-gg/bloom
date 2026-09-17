#include "support.hpp"
#include <bloom/media/provider/registry.hpp>
using namespace bloom::media::provider;
int main() {
    try {
        CapabilityRegistry registry;
        ProviderDeclaration d{test::capability(), test::execution(), test::evidence()};
        test::check(std::holds_alternative<Digest>(registry.registerProvider(d)),
                    "fake registration");
        PipelineQualificationV1 p;
        p.steps = {{std::get<Digest>(digest(d.capability)), std::get<Digest>(digest(d.execution)),
                    std::get<Digest>(digest(d.evidence))}};
        p.profile = "test";
        p.reopenPolicy = "none";
        p.qcProfile = "none";
        p.result = QcResult::Pass;
        test::check(std::holds_alternative<Unavailable>(registry.begin(p)),
                    "component qualification is not transitive");
        test::check(std::holds_alternative<Digest>(registry.qualifyPipeline(p)),
                    "exact pipeline fixtures");
        test::check(std::holds_alternative<Unavailable>(registry.begin(p, "apple-authorized")),
                    "technical qualification is not authority");
        auto attempt = std::get<MediaAttempt>(registry.begin(p));
        auto changed = p;
        changed.profile = "other";
        test::check(std::holds_alternative<Unavailable>(registry.begin(changed)),
                    "no wildcard profile");
        changed = p;
        changed.purpose = Purpose::Delivery;
        test::check(std::holds_alternative<Unavailable>(registry.begin(changed)),
                    "no purpose substitution");
        auto replacement = d;
        ++replacement.execution.generation;
        test::check(std::holds_alternative<Digest>(registry.registerProvider(replacement)),
                    "new generation registered");
        registry.revoke(d.execution);
        test::check(std::holds_alternative<Unavailable>(registry.begin(p)),
                    "revoked generation unavailable to new attempts");
        test::check(attempt.accepts(0, d.execution) && !attempt.accepts(0, replacement.execution),
                    "attempt pins immutable execution");
        auto sharedAttempt = std::make_unique<MediaAttempt>(attempt);
        attempt.terminate();
        test::check(!sharedAttempt->accepts(0, d.execution), "copies share terminal attempt state");
        test::check(!attempt.accepts(0, d.execution), "terminated attempt stays terminal");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
