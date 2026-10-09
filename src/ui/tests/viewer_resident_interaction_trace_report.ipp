// Private harness trace summary; metadata/counters are separate from compositor evidence.
if (trace && !options.trace.empty()) {
    verifyAndExportTrace(checks, trace, options.trace);
    std::cout << (options.baseline ? "BASELINE" : "TRANSITIONS")
              << " output_format=1920x1080 par=4/3 requested_host=960x640 native_viewport="
              << nativeViewportSize.width() << 'x' << nativeViewportSize.height()
              << " device_pixel_ratio=1 bridge_poll_ms=" << bridge.pollingInterval().count()
              << " foreground_poll_ms=4 scheduler=defaults resolution_policy="
              << static_cast<unsigned>(controller.settings().resolutionPolicy) << " validation="
              << (qEnvironmentVariable("VK_INSTANCE_LAYERS").contains("validation") ? 1 : 0)
              << " request_byte_limit=" << controller.effectivePixelStorageByteLimit()
              << " cache_byte_budget=" << frameCache->byteBudget()
              << " native_dispatches=" << service.status().counters.nativeDispatches
              << " gpu_cache_hits=" << service.status().counters.gpuCacheHits
              << " gpu_cache_misses=" << service.status().counters.gpuCacheMisses
              << " cpu_fallbacks=" << service.status().counters.cpuFallbacks
              << " full_frame_readbacks=" << service.status().counters.fullFrameReadbacks << '\n';
}
