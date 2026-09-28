#ifdef _WIN32
#include <windows.h>

#include <fstream>
#include <cstdio>
#include <string>

// The sender's encoder probe must finish, while its real transport child
// deliberately ignores stdin EOF so the stop path has to terminate it.
int main(int argc, char** argv) {
  if (::GetEnvironmentVariableA("COREVIDEO_TEST_SRT_FAULT", nullptr, 0) > 0) {
    const char* input = "";
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::string(argv[i]) == "-i") input = argv[i + 1];
    }
    std::fprintf(stderr, "[h264] error while decoding MB 12 7 %s\n", input);
    std::fprintf(stderr, "[mpegts] Packet corrupt (stream = 0)\n");
    std::fflush(stderr);
    ::Sleep(250);
    return 1;
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-frames:v") return 0;
  }
  char marker[MAX_PATH]{};
  if (::GetEnvironmentVariableA("COREVIDEO_TEST_FFMPEG_PID_FILE", marker, MAX_PATH) > 0) {
    std::ofstream output(marker, std::ios::trunc);
    output << ::GetCurrentProcessId();
  }
  ::Sleep(30000);
  return 0;
}
#endif
