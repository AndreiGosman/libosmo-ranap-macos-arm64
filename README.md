# libosmo-ranap for macOS ARM64

[osmo-iuh](https://osmocom.org/projects/osmohnbgw/wiki) library tree
port to macOS ARM64 (Apple Silicon), Darwin 26+. The tree builds four
shared libraries for the 3G Iu interfaces: libosmo-ranap (RANAP, the
Iu-CS and Iu-PS control plane between the core network and the RNC or
Home NodeB gateway), libosmo-hnbap (HNBAP, Home NodeB registration),
libosmo-rua (RUA, RANAP transport over Iuh) and libosmo-sabp (SABP,
cell broadcast). The repository is named after libosmo-ranap because
that library is what osmo-msc and osmo-sgsn link when built with
`--enable-iu`. osmo-hnbgw, the daemon that used to live in this tree,
is a separate upstream repository since 1.5 and is not ported here.

Upstream version: 1.8.1. Three patches applied, two in the build
system and one in the testsuite; no library source file is changed.
Testsuite on macOS 26.6.2, Apple M5 Pro: 3 of 3 pass (helpers, hnbap,
ranap).

Upstream README preserved as [README.upstream.md](README.upstream.md).

## Prerequisites

- [libosmocore](https://github.com/AndreiGosman/libosmocore-macos-arm64) >= v0.2.4 (upstream 1.14.2; configure asks for >= 1.12.0)
- [libosmo-netif](https://github.com/AndreiGosman/libosmo-netif-macos-arm64) >= v0.1.2 (upstream 1.8.0)
- [libosmo-sigtran](https://github.com/AndreiGosman/libosmo-sigtran-macos-arm64) >= v0.1.0 (upstream 2.3.0; configure asks for >= 2.2.0, which is why libosmo-sccp is not a separate dependency)
- [libasn1c](https://github.com/AndreiGosman/libasn1c-macos-arm64) >= v0.1.0 (upstream 0.9.39)
- [libsctp-compat](https://github.com/AndreiGosman/libsctp-compat-macos-arm64) >= v0.3.1, for `netinet/sctp.h` and `-lsctp` in the testsuite
- python3 from Homebrew, for `asn1/utils/asn1tostruct.py`, which generates the encoder and decoder files at build time
- autoconf, automake, libtool and pkg-config from Homebrew

The asn1c compiler is not needed. The asn1c-generated files for the
four protocols are committed upstream (593 of them for RANAP alone);
only `make regen` would call asn1c, and that target is untouched.

## Build

```bash
git clone https://github.com/AndreiGosman/libosmo-ranap-macos-arm64.git
cd libosmo-ranap-macos-arm64
autoreconf -fi
mkdir -p build && cd build
../configure --prefix=$HOME/sdr-lab/local \
  CFLAGS=-I$HOME/sdr-lab/local/include LDFLAGS=-L$HOME/sdr-lab/local/lib
make -j$(sysctl -n hw.ncpu)
make check
make install
pkg-config --modversion libosmo-ranap   # 1.8.1
```

`CFLAGS` and `LDFLAGS` point configure at the libsctp-compat shim:
`configure.ac` stops with "netinet/sctp.h not found" otherwise, since
the header check runs outside pkg-config. This is the same line the
osmo-msc port uses. `--disable-doxygen` is not an osmo-iuh option.

The install lands in `lib/libosmo-{ranap,hnbap,rua,sabp}.dylib`, the
matching `lib/pkgconfig/*.pc`, and `include/osmocom/{ranap,hnbap,rua,
sabp,iuh}/`, 601 headers under `ranap/`. After patch 002,
`pkg-config --libs libosmo-hnbap` prints `-losmo-hnbap -losmo-ranap`.

## Patches applied

| Patch | Upstream file | Issue | Fix |
|-------|---------------|-------|-----|
| 001 | `src/Makefile.am` | The `gen_*.stamp` rules post-process the asn1tostruct.py output with `sed -i 'script' files`; BSD sed takes the argument after `-i` as a backup suffix and the build stops in `src/` before any object is compiled | Loop over the files and write each substitution through a temporary file; same result with GNU sed |
| 002 | `src/Makefile.am`, `libosmo-{hnbap,rua,sabp}.pc.in` | The three libraries use `asn1_xer_print` and `talloc_asn1_ctx`, defined in `iu_helpers.c` of libosmo-ranap; GNU ld leaves the reference unresolved despite `-no-undefined`, ld64 refuses to link | Add `libosmo-ranap.la` to their `LIBADD` and `Requires: libosmo-ranap` to their pkg-config files; libosmo-ranap references none of their symbols, so there is no cycle |
| 003 | `tests/test-helpers.c`, `tests/test-helpers.ok` | `AF_X25` does not exist on Darwin (the family is `AF_CCITT`), and the expected output hard-codes `AF_INET6` as 10 where Darwin has 30 | Define `AF_X25` as `AF_CCITT` when missing; print the address family by name and update the expected output |

A fourth commit tracks `.tarball-version` with the upstream version so
that `pkg-config --modversion libosmo-ranap` reports 1.8.1 rather than
the fork's tag. osmo-msc 1.16.0 requires exactly `libosmo-ranap >=
1.8.1`, so this matters for its `--enable-iu` configure.

All three patches have no effect on GNU/Linux beyond an explicit
`DT_NEEDED` entry from patch 002, and are worth sending upstream. The
`regen` targets in `src/*/Makefile.am` still use `sed -i` in the GNU
form; they are only run to regenerate the tree from ASN.1 sources with
asn1c installed and were left alone.

What needed no patch: the RANAP, HNBAP, RUA and SABP codecs
(asn1c-generated C on the libasn1c runtime), the Iu client in
`iu_client.c` (SCCP through the libosmo-sigtran port), and the VTY
node. None of the GNU-isms found in earlier ports of this series
(errno aliases, `sched_setscheduler`, `SOCK_SEQPACKET`, timerfd
semantics, `-lrt`, `gethostbyname_r`) appear in this tree. The 76
`-Wparentheses-equality` warnings come from the generated ASN.1 code
and show with clang on any host.

## Not covered

No RNC, Home NodeB or HNB-GW was connected, so RANAP over a live Iu
link was not exercised; the testsuite covers encoding and decoding of
RANAP and HNBAP messages and the transport-layer helpers. `hnb-test`
builds as part of `make check` and was not run against a peer.
osmo-hnbgw and osmo-sgsn are not ported.

## Dependency cascade

Ports enabled by this repository:

- osmo-msc rebuilt with `--enable-iu`: Iu-CS towards a Home NodeB gateway
- osmo-hnbgw, when ported: the Iuh side, HNBAP and RUA, towards a 3G femtocell
- osmo-sgsn with `--enable-iu`: Iu-PS

## Prior ports in this series

1. libosmocore-macos-arm64 v0.2.4
2. libsctp-compat-macos-arm64 v0.3.1
3. srsRAN-4G-macos-arm64 v0.1.0
4. kraken-macos-arm64
5. libosmo-netif-macos-arm64 v0.1.2
6. libosmo-abis-macos-arm64 v0.1.1
7. libosmo-sigtran-macos-arm64 v0.1.0
8. osmo-hlr-macos-arm64 v0.1.0
9. osmo-mgw-macos-arm64 v0.1.0
10. libsmpp34-macos-arm64 v0.1.0
11. osmo-msc-macos-arm64 v0.1.0
12. osmo-bsc-macos-arm64 v0.1.0
13. osmo-trx-macos-arm64 v0.1.0
14. osmo-bts-macos-arm64 v0.1.0
15. libasn1c-macos-arm64 v0.1.0
16. This repository

## License

As upstream, per `debian/copyright`: AGPL-3.0-or-later for the tree,
GPL-2.0 for the Eurecom-derived RUA encoder and decoder templates
under `asn1/rua/eurecom/`. The patches in this repository touch the
build system and the testsuite and carry the same license.

## Credits

Port developed by Andrei Gosman ([@agoarchitecture](https://linkedin.com/in/agoarchitecture))
in collaboration with Claude Code CLI (Anthropic). All commits authored by
Andrei; Claude assisted with pattern analysis, debugging, and iteration.
