#include "ffmpeg_provider.hpp"
int main(int argc, char** argv) {
    return argc == 2 && bloom::media::ffmpeg::generateFixtures(argv[1]) ? 0 : 1;
}
