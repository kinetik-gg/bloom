# Reason-checked mutation proof. Building the synthetic unclassified alternative must fail, and the
# diagnostic must be the missing GpuOperationCoverageTraits specialization -- not any other error.
# The positive probe is built first to prove the contract header itself compiles in this build.
#
# Required variables:
#   BLOOM_BUILD_DIR     build tree to drive
#   BLOOM_PROBE_POSITIVE positive object target that must compile
#   BLOOM_PROBE_NEGATIVE negative object target that must fail to compile

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

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${BLOOM_BUILD_DIR}" --target "${BLOOM_PROBE_NEGATIVE}"
    RESULT_VARIABLE negative_result
    OUTPUT_VARIABLE negative_out
    ERROR_VARIABLE negative_err
)
set(negative_log "${negative_out}\n${negative_err}")
if(negative_result EQUAL 0)
    message(FATAL_ERROR
        "negative probe compiled; an unclassified pixel alternative would escape the contract")
endif()
if(NOT negative_log MATCHES "GpuOperationCoverageTraits")
    message(FATAL_ERROR
        "negative probe failed without naming the missing coverage trait:\n${negative_log}")
endif()
if(NOT negative_log MATCHES "SyntheticUnclassifiedOperation")
    message(FATAL_ERROR
        "negative probe failed for an unrelated reason (not the synthetic alternative):\n"
        "${negative_log}")
endif()
message(STATUS
    "mutation proof: positive probe compiles; negative probe fails on the missing trait")
