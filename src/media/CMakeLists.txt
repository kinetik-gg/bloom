# Image and audio pipelines have separate targets and source ownership. Keep this parent
# intentionally narrow: it only lists the subdirectories, so media is never a second
# composition root.
add_subdirectory(image)
add_subdirectory(audio)
