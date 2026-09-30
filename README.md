# hnb-test branch of libosmo-ranap for macOS ARM64

This branch turns `tests/hnb-test` of the osmo-iuh tree into a small
test RNC for the 3G core: it registers with osmo-hnbgw over Iuh, then
plays the UE side of a Location Update on Iu-CS and of a GMM attach,
Service Request, PDP context activation and a real Iu-PS user plane on
Iu-PS. The user plane is a GTP-U direct tunnel to the GGSN in Iu UP
transparent mode (3GPP TS 25.415 4.2.2, no Iu UP frame), which is what
osmo-sgsn asks for.

The branch is based on osmo-iuh upstream master (8271c81, 30 September
2026), not on the 1.8.1 tag that `main` of this repository uses. That
base already contains four hnb-test commits from this work, merged
upstream in September 2026, and the libosmo-ranap link fix that `main`
carries as patch 002. `main` is untouched by this branch; the port
README there is [README.md on main](https://github.com/AndreiGosman/libosmo-ranap-macos-arm64/blob/main/README.md).
The upstream README of the base is preserved as
[README.upstream.md](README.upstream.md).

## Composition

Nine commits above upstream master, in order:

| Commit | Kind | What it does |
|--------|------|--------------|
| build: rewrite generated includes without sed -i | Darwin build, same as `main` patch 001 | BSD sed takes the argument after `-i` as a backup suffix; loop over the generated files and write each substitution through a temporary file |
| tests: make test-helpers independent of Linux address family constants | Darwin test, same as `main` patch 003 | `AF_X25` is `AF_CCITT` on Darwin and `AF_INET6` is 30, not 10 |
| build: track .tarball-version for stable version reporting | Version | `1.8.1.5-8271`, what `git-version-gen` derives from the base; without it a branch tag would leak into `pkg-config --modversion` |
| hnb-test: add GMM Attach Request, Identity, Auth/Ciph handling and Attach Complete on Iu-PS | hnb-test 0005 | GMM attach against osmo-sgsn: Identity Response (IMEI), Authentication and Ciphering Response with the 8-byte UMTS RES and the IMEISV IE, Attach Complete; P-TMSI kept |
| hnb-test: add Service Request, Activate PDP Context Request and a RAB Assignment Response stub | hnb-test 0006 | osmo-sgsn releases Iu after Attach Complete, so PDP activation opens a new connection with a GMM Service Request and sends the SM request on Service Accept; the first RAB Assignment handling was a FailedList stub |
| hnb-test: use RAN-side RANAP decoder for CO messages from the CN | hnb-test 0007 | `ranap_cn_rx_co()` has no RAB Assignment case ("Decode not implemented"); a RAN decodes with `ranap_ran_rx_co()` |
| hnb-test: set up the Iu-PS RAB with a GTP-U direct tunnel to the GGSN | hnb-test 0008 | Decodes RAB-SetupOrModifyItemFirst, accepts transparent mode with a GTP TEI, opens a UDP socket on port 2152, answers with a SetupOrModifiedList (own address in the request's X.213 NSAP or plain encoding, own TEID), stores the PDP address, adds `channel ps ping A.B.C.D` and prints downlink T-PDUs |
| iu_client: report a failed RAB from the RAB Assignment Response | library, same as `main` patch 004 | The FailedList of a RAB Assignment Response was dropped with rc -1; now decoded and submitted as `RANAP_IU_EVENT_RAB_ASSIGN_FAIL`; needed by osmo-sgsn patch 005 in [osmo-sgsn-macos-arm64](https://github.com/AndreiGosman/osmo-sgsn-macos-arm64) v0.1.1 |
| docs: README for the hnb-test branch | This file | |

The four hnb-test commits already upstream (N(SD)=1 on the MM
Authentication Response, full UMTS RES with NAS routing by CN domain,
LU Accept IEs after the LAI, N(SD) counted over all uplink MM messages)
are not repeated here; the `tests/` directory of this branch is
byte-identical to the tree the results below were obtained with.

None of the hnb-test commits is Darwin-specific. They are candidates
for Gerrit as a series after review; the commit messages here are
longer than the 50-character subjects Gerrit wants and would be
shortened then.

## Build

Same as `main`, with the prerequisites listed there (libosmocore,
libosmo-netif, libosmo-sigtran, libasn1c, libsctp-compat, python3,
autotools from Homebrew):

```bash
git clone -b hnb-test https://github.com/AndreiGosman/libosmo-ranap-macos-arm64.git
cd libosmo-ranap-macos-arm64
autoreconf -fi
mkdir -p build && cd build
../configure --prefix=$HOME/sdr-lab/local \
  CFLAGS=-I$HOME/sdr-lab/local/include LDFLAGS=-L$HOME/sdr-lab/local/lib
make -j$(sysctl -n hw.ncpu)
make -C tests hnb-test
./tests/hnb-test
```

`hnb-test` is a `noinst` program; the binary is
`build/tests/.libs/hnb-test` behind a libtool wrapper. `make install`
installs the four libraries as on `main`, including the `iu_client.c`
fix.

## Running hnb-test

Options: `-u N` number of UEs to announce, `-g ADDR` HNB-GW address
(default 127.0.0.1, Iuh SCTP port 29169), `-G ADDR` local address for
the GTP-U socket (default 127.0.0.12, must exist on the host; on macOS
add a loopback alias with `ifconfig lo0 alias 127.0.0.12`). With
libsctp-compat the process needs its own UDP encapsulation port and the
port of osmo-hnbgw, for example
`LIBSCTP_COMPAT_UDP_ENCAPS_PORT=9894 LIBSCTP_COMPAT_UDP_ENCAPS_REMOTE_PORT=9896`.

The VTY listens on 127.0.0.1:2324. The commands, in the order of a
session:

```
hnbap hnb register
ranap reset cs
ranap reset ps
hnbap ue register IMSI
channel cs lu imsi IMSI
channel ps attach imsi IMSI
channel ps pdp-activate imsi IMSI apn APN
channel ps ping A.B.C.D
```

The subscriber the test answers for uses K = 000102030405060708090a0b0c0d0e0f
with OP = 0 (Milenage, `hnb_test_subscr_key` in `tests/hnb-test.c`),
so the HLR entry needs `aud3g milenage k 000102030405060708090a0b0c0d0e0f op 00000000000000000000000000000000`
(the subscriber used for the results below also had `aud2g comp128v1`
with the same key). Progress is printed on stdout: received NAS messages,
the RAB Assignment decode, the GTP-U endpoints and every T-PDU.

What the Osmocom core demands from a UE, which is why these commits
exist: osmo-msc on UTRAN accepts only UMTS AKA (the Authentication
Response carries the 8-byte RES, 4 in the SRES field and 4 in the
extension IE); N(SD) counts per connection over all uplink MM messages;
osmo-sgsn asks Identity Request (IMEI) first and requires the IMEISV IE
in the Authentication and Ciphering Response; osmo-sgsn releases the Iu
connection right after Attach Complete, so PDP activation starts with a
GMM Service Request on a new connection; the RAB Assignment Request
carries the GGSN GTP-U endpoint (Direct Tunnel), and the SGSN forwards
the RNC endpoint from the response to the GGSN in an Update PDP Context
Request before it sends Activate PDP Context Accept.

## Verified

On one macOS host, without radio, against the osmo-hlr, osmo-stp,
osmo-mgw, osmo-msc (Iu-CS), osmo-sgsn (Iu-PS, patch 005), osmo-ggsn
(root, for the tun device) and osmo-hnbgw ports of this series:

- HNB register, RANAP RESET on both domains, UE register.
- Iu-CS: Location Updating Accept with TMSI after UMTS AKA and Security
  Mode Command.
- Iu-PS: Attach Accept with P-TMSI, Service Accept, Create PDP Context
  Request/Response between SGSN and GGSN, RAB Assignment
  Request/Response, Update PDP Context Request/Response, Activate PDP
  Context Accept with an IPv4 PDP address from the GGSN pool.
- User plane: `channel ps ping` sends an ICMP echo request from the PDP
  address as a G-PDU to the GGSN TEID; the GGSN writes it to its tun
  device, the host answers, and the echo reply comes back as a G-PDU on
  the hnb-test TEID and is printed decoded. The reply carries the GTP-U
  S flag, which is why the receiver subtracts the four optional header
  octets from the length field (TS 29.281 5.1).
- Failure path: with `-G` set to an address the host does not have,
  the bind fails, hnb-test answers a FailedList, and the SGSN with
  patch 005 sends Activate PDP Context Reject and Delete PDP Context;
  the PDP context is gone on both GSNs.

## Not covered

One RAB per RAB Assignment Request (osmo-sgsn sends one). IPv4 only.
The GTP-U socket and the RAB state are not reset on Iu Release or RAB
Release; a second PDP activation in the same process reuses them.
Support mode Iu UP and a binding ID transport association are refused
with a FailedList, on purpose. No radio side.

## License

As upstream, per `debian/copyright`: AGPL-3.0-or-later for the tree,
GPL-2.0 for the Eurecom-derived RUA encoder and decoder templates under
`asn1/rua/eurecom/`. The commits on this branch carry the same license.

## Credits

Port developed by Andrei Gosman ([@agoarchitecture](https://linkedin.com/in/agoarchitecture))
in collaboration with Claude Code CLI (Anthropic). All commits authored by
Andrei; Claude assisted with pattern analysis, debugging, and iteration.
