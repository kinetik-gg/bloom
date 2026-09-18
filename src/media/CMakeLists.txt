# Image, audio and disk-cache pipelines have separate targets and source ownership. Keep this
# parent intentionally narrow: it only lists the subdirectories, so media is never a second
# composition root.
add_subdirectory(image)
add_subdirectory(audio)
add_subdirectory(cache)

add_subdirectory(provider)

add_subdirectory(video)
