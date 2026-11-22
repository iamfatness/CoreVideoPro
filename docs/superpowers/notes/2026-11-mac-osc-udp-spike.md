# Mac OSC UDP spike: Network.framework vs BSD socket (plan 2026-11-16 T16)

Decision: **BSD datagram socket on a DispatchSourceRead.** Network.framework failed the rule.

Harness case `control/udp-spike` (`ControlUdpSpikeTests.swift`), NWListener(.udp) on 127.0.0.1:0,
echoing on the per-sender NWConnection; clients are BSD sockets. Apple Silicon, macOS 26.3, machine
heavily loaded by other agents (load average 40-85). Three runs, 2026-11-16, each ~6.6 s:

| # | Check | Run 1 | Run 2 | Run 3 | Pass |
|---|---|---|---|---|---|
| 1 | Reply source port = listener port | pass | pass | pass | yes |
| 2 | 16 senders, exactly 1 reply each within 1 s | **fail** (counts 2,1,1,1,1,1,0,…: one reply landed on the wrong client) | pass | pass | **no** |
| 3 | 3 server-initiated datagrams arrive from the listener port | pass | pass | pass | yes |
| 4 | Dead sender (ICMP unreachable); B still answered; listener stays ready | pass | pass | pass | yes |
| 5 | Burst of 1000 x 64 B, >= 990 received | 1000 | 1000 | 1000 | yes |
| 6 | 65,000-byte datagram intact or refused; normal datagram still echoes | **fail** | **fail** | **fail** | **no** |

Check 6 detail: `sendto` reported 65,000 bytes sent; NWConnection.receiveMessage delivered **9,216**
bytes with no error (silent truncation). A plain BSD socket on the same machine receives the same
65,000-byte datagram intact (python check, `net.inet.udp.maxdgram` = 9216 limits only the default
send buffer). Silent truncation would hand the OSC decoder a corrupt packet rather than an error.

The checks can fail: replying through a second listener's connections reds check 1 (no reply
arrives from the listener port); dropping the echo for odd client ports reds check 2.

Outcome: T19 implements `ControlOscServer` on a BSD socket; the spike case is unregistered now and
deleted in T19.
