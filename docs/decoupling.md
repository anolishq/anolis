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

- **The runtime contains no workbench references at all.** `grep -ri workbench core/ src/` returns nothing.
- **`install.sh` mentions the workbench only in comments**, describing what it replaces or mirrors. No code path depends on it.
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
topologies are first-class and composable; the system supports all three and
crowns none:

| Topology | Bind and auth | Deploy artifact |
| --- | --- | --- |
| **Loopback on-device** | `127.0.0.1`, auth-exempt | `--project` online, or `--local` offline |
| **LAN with auth** | non-loopback with a generated token | either |
| **Fully air-gapped** | loopback | staged bundle, carried to the target |

### The LAN topology is guarded at two layers

A non-loopback bind with authentication disabled is refused **by the runtime at
startup** — it will not serve an unauthenticated control API to the network. An
explicit `http.allow_insecure_bind` exists as a deliberate override, and it is the
only way to get that combination.

`install.sh` **mirrors that same guard as an install-time preflight**, so the
failure surfaces as an actionable install error rather than a service that
crash-loops while the install reports success. That mirroring is itself worth
preserving: the two checks are meant to agree, and a change to one needs the
other.

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
