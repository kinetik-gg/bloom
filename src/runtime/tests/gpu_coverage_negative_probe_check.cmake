# Reason-checked mutation proof. The positive probe must compile first (the contract header is valid
# in this build). Then each negative probe must fail to compile, and its diagnostic must name the
# expected missing classification -- never some unrelated error. Three negative probes cover the
# three compile-time escape routes: a new CompiledOperation alternative, a new ImageEffectKernel
# alternative, and a new feature enumerator guarded by -Werror=switch. (A new built-in pixel node is
# a runtime registry mutation proved inside the coverage gate itself.)
#
# Required variables:
#   BLOOM_BUILD_DIR                 build tree to drive
#   BLOOM_PROBE_POSITIVE            positive object target that must compile
#   BLOOM_PROBE_NEGATIVE_OPERATION  operation negative target; REASON_OPERATION_A/B must match
#   BLOOM_PROBE_NEGATIVE_EFFECT     effect negative target;    REASON_EFFECT_A/B must match
#   BLOOM_PROBE_NEGATIVE_FEATURE    feature negative target;   REASON_FEATURE_A/B must match

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${BLOOM_BUILD_DIR}" --target "${BLOOM_PROBE_POSITIVE}"
    RESULT_VARIABLE positive_result
    OUTPUT_VARIABLE positive_out
    ERROR_VARIABLE positive_err
)
if(NOT positive_result EQUAL 0)
    message(FATAL_ERROR
        "positive contract probe did not compile; the header is broken independently:\n"
        "${positive_out}\n${positive_err}")
endif()

function(check_negative_probe target reason_a reason_b)
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${BLOOM_BUILD_DIR}" --target "${target}"
        RESULT_VARIABLE negative_result
        OUTPUT_VARIABLE negative_out
        ERROR_VARIABLE negative_err
    )
    set(negative_log "${negative_out}\n${negative_err}")
    if(negative_result EQUAL 0)
        message(FATAL_ERROR
            "negative probe ${target} compiled; an unclassified pixel item would escape the "
            "contract")
    endif()
    if(NOT negative_log MATCHES "${reason_a}")
        message(FATAL_ERROR
            "negative probe ${target} failed without naming '${reason_a}':\n${negative_log}")
    endif()
    if(NOT negative_log MATCHES "${reason_b}")
        message(FATAL_ERROR
            "negative probe ${target} failed for an unrelated reason (not '${reason_b}'):\n"
            "${negative_log}")
    endif()
    message(STATUS "mutation proof: ${target} fails on the expected reason")
endfunction()

check_negative_probe("${BLOOM_PROBE_NEGATIVE_OPERATION}" "${BLOOM_REASON_OPERATION_A}"
                     "${BLOOM_REASON_OPERATION_B}")
check_negative_probe("${BLOOM_PROBE_NEGATIVE_EFFECT}" "${BLOOM_REASON_EFFECT_A}"
                     "${BLOOM_REASON_EFFECT_B}")
check_negative_probe("${BLOOM_PROBE_NEGATIVE_FEATURE}" "${BLOOM_REASON_FEATURE_A}"
                     "${BLOOM_REASON_FEATURE_B}")

message(STATUS
    "mutation proof: positive probe compiles; operation, effect, and feature probes fail on the "
    "expected reasons")
