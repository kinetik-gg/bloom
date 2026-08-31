# One recipe file per direct dependency, per docs/architecture/dependency-intake.md. Each recipe
# declares its exact locked archive URL, SHA-256, enabled feature set, and install behavior.
include(yyjson.cmake)
include(zlib.cmake)
include(libzip.cmake)
include(imath.cmake)
include(libdeflate.cmake)
include(openexr.cmake)
