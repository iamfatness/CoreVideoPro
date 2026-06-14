#include "core/MediaCore.h"
#include "rpc/JsonRpcServer.h"

#include <iostream>

int main() {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  server.run(std::cin, std::cout);
  return 0;
}
