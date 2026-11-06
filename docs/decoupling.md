# Decoupling

Anolis is not a turnkey product with one deployment pipeline. It is a **decoupled
component ecosystem joined only by stable, lossless contracts** — closer to the
ROS model than the appliance model.

Each component is independently buildable, testable, runnable and correct on its
own. The rigour of the system lives in two places and only two:

1. **Component independence** — each component stands alone and is correct standing alone.
2. **Contract fidelity** — the artifacts passed between components are lossless and stable.

Everything else, topology included, is the integrator's free choice. The runtime
may run on the Pi with providers as child processes; the workbench may run on the
Pi, on a laptop, or not at all; observability may be native, containerised, or
absent. None of these is *the* path.

## The decoupling invariant

> The deployed machine is the output of `install.sh` **alone**. Delete the
> workbench and the machine must still **install**, **run**, be **operated**, and
> be **safe**. Anything the machine *needs* that lives *only* in the workbench is
> a bug.

This is the test to apply to any proposed change. It is not aspirational — it
holds today, and it is cheap to re-check:

- **The runtime has no functional workbench dependency.** `grep -ri workbench core/` finds two prose mentions only: a warning string in `core/runtime/config.cpp` and a comment in `core/control/latch_store.hpp`.
- **`install.sh` mentions the workbench mostly in comments**, describing what it replaces or mirrors. One code path names it: `--uninstall` removes the workbench's copy of the runtime token (`/etc/anolis/workbench-runtime-token`) if present. No install or run path depends on it.
- **The standing operator control surface is the runtime's `/v0` HTTP API.** The workbench proxies it; it does not extend it. An operator with `curl` can do what the UI does.

A workbench *surface* is therefore additive, never load-bearing. The distinction
matters: building a nicer way to do something is not coupling, but making that
the only way is.

## The provisioning consequence

There is exactly one provisioning engine, `install.sh`. The CLI wrapper and the
browser UI both hand it a config directory and let it work — see
[deploy/README.md](deploy/README.md). A machine provisioned through the UI is
indistinguishable from one provisioned by hand, which is what makes the invariant
checkable rather than merely stated.

## Air-gap is the baseline, not a requirement

"Real facilities do not connect equipment to the internet" is a **conservative
baseline assumption, not an absolute.** The design must not *require* the
internet — but it must not assume its absence past the point of usefulness
either.

Critically, **no-internet is not no-network.** A facility typically has an
internal LAN, and the `/v0` API is fully functional over it. So three operate
topologies are first-class and composable; the system supports all three. The
installer renders every bundle, staged ones included, for the LAN topology: an
unquoted `bind: 127.0.0.1` becomes `bind: 0.0.0.0`, and `auth_enabled: true` is
added unless the config already sets `auth_enabled`. To keep a loopback bind,
leave `bind` out (the runtime default is `127.0.0.1`) or edit `runtime.yaml`
after install:

| Topology | Bind and auth | Deploy artifact |
| --- | --- | --- |
| **Loopback on-device** | `127.0.0.1` (the runtime default); with auth on, local clients send the token | `--project` online, or `--local` offline |
| **LAN with auth** | non-loopback with a generated token | either |
| **Fully air-gapped** | loopback | staged bundle (`--stage`), carried to the target and installed with `--local` |

### The LAN topology is guarded at two layers

A non-loopback bind with authentication disabled is refused **by the runtime at
startup** — it will not serve an unauthenticated control API to the network. An
explicit `http.allow_insecure_bind` exists as a deliberate override, and it is the
only way to get that combination.

`install.sh` **mirrors that same guard as an install-time preflight**, so the
failure surfaces as an actionable install error rather than a service that
crash-loops while the install reports success. That mirroring is itself worth
preserving: the two checks are meant to agree, and a change to one needs the
other. The preflight reads `http` as the runtime does, without python: a block
mapping with quoted or plain values, every boolean spelling the runtime
accepts (`true`, `yes`, `on`, `y` and their negatives, lower case, upper case
or capitalised), and it skips the bind check when `http.enabled` is false. An
`http.enabled`, `http.auth_enabled` or `http.allow_insecure_bind` that is
present but not one of those booleans, an empty value included, fails the
preflight, as it fails the runtime. Only an absent `http.bind` is the loopback
default: `bind: ""` is checked as the non-loopback address it is.

Its reader is not a YAML parser. It reads a strict subset of YAML: enough for
the configs `install.sh` writes, and for the sequences PyYAML writes at their
key's column, as the workbench saves projects. It refuses everything else
with `cannot verify the http settings in <file>: <construct> is not supported
by the installer's reader` rather than guess. The subset:

- at most one `---`, on the first content line, with nothing after it but a
  comment (`--- # c`); no `...`, no `%` directive, no byte-order mark, no NUL
  byte, and LF or CRLF line endings;
- at column 0, a name followed by a colon: a letter or `_`, then letters,
  digits, `_` or `-`. A sequence entry (`- `) may also sit at column 0 as the
  value of the key just above it, when that key is not `http` and has nothing
  after its colon but a comment. That is how PyYAML, and so the workbench,
  writes `providers:` and its `- id: ...` entries. Nothing else may be at
  column 0;
- `http:` once, in block style, its children at one indentation, each a name
  as above and a colon. A sequence entry may sit at that indentation as the
  value of the child just above it, when that child is not one of the keys
  below and has nothing after its colon but a comment (`cors_allowed_origins:`
  then `- http://...`). A flow mapping or sequence (`http: {...}`,
  `http: [...]`), or a sequence as `http`'s own value, is refused whatever it
  holds: write `http:` in block style;
- `enabled`, `bind`, `port`, `auth_enabled` and `allow_insecure_bind` written as
  a plain value or a value quoted on one line, with no escape but `\"` and `\\`,
  no anchor, alias, tag, block scalar, collection or sequence entry, nothing
  after the closing quote but a comment, and nothing on the lines under them;
  `port` a plain decimal number. A plain value is the text after the colon, up
  to a comment, that does not start with a quote, `[`, `{`, `&`, `!`, `*`, `|`,
  `>`, or a `-` followed by a blank or the end of the line, and holds no `:`
  followed by a blank or the end of the line;
- everywhere: no tab in the indentation, and no quote or bracket left open at
  the end of a line (a tag or anchor in front of a quoted value counts), except
  inside a block scalar (`|`, `>`) after a key that is a name, whose lines are
  passed over. Its lines must be indented past that key (under `- key: |`,
  past `key`, two columns right of the dash), none less than its first, and it
  has no indentation indicator (`|4`). No quoted mapping key (`"a": b`,
  `'a':'b'`); no alias (`*name`); no tag or anchor written with a quote or a
  flow character (`"'[]{},`); no explicit key (`?` followed by a space); and
  no NEL, line separator or paragraph separator character; comments on their
  own line or after a value.

Quoted key names, `?` keys, merge keys and an indented first line are outside
it. Simplify the config, or check it with
`anolis-runtime --check-config <file>`.

The API token is generated **per device at install time**, never baked into a
bundle — a bundle is a redistributable artifact that may be installed on many
machines. It is reused when already present, so upgrading does not invalidate
tokens already issued to clients.

Note what this does *not* give you: on a non-loopback bind without TLS, the
bearer token crosses the network in plaintext. The runtime warns about exactly
that. Treat the LAN topology as "authenticated", not "confidential".

## Applying the invariant

When a change is proposed, ask:

- Could the machine still be installed, run, operated and made safe with this component deleted?
- Does the artifact passed across this boundary lose anything the receiver needs?
- Is this the *only* way to do something, or an additional way?

A "no" to the first, or a "yes" to the second, is the bug. The third is usually
the early warning.
