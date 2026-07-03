#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/JsonRpcServer.h"

#include <iostream>

int main() {
  // The live server runs the audio/output worker, so it needs the encoder wrapped
  // in AsyncEncoderSink (non-blocking submit + drop-to-latest). Tests construct
  // MediaCore with createDefaultModules and keep the synchronous encoder.
  corevideo::core::MediaCore mediaCore(corevideo::modules::createLiveServerModules());
  corevideo::rpc::JsonRpcServer server(mediaCore);
  server.run(std::cin, std::cout);
  return 0;
}
