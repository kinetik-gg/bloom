# INSTANCE-0 throwaway spike. Reached only when BLOOM_BUILD_SPIKES is ON, which it is not by
# default, so the product build never compiles or links this target.
add_executable(bloom_instance0_spike instance0_spike.cpp)
target_link_libraries(bloom_instance0_spike PRIVATE bloom_runtime_evaluator)
bloom_enable_warnings(bloom_instance0_spike)
bloom_enable_strict_floating_point(bloom_instance0_spike)
