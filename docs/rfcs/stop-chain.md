# RFC: observing the emergency stop (stop chain)

Status: increments 1 to 5 are implemented, 1 to 3 in anolis 0.1.53
(#488, #489, #490), 4 in 0.1.55 (#503), and 5 in anolis-workbench 0.22.0
(anolishq/anolis-workbench#641, anolishq/anolis-workbench#644). Increment 6,
renaming `/v0/estop`, is not (#485). Tracked as anolishq/anolis#284, part of
anolishq/anolis#283. Depends on the stop categories in
[safety.md](../safety.md#stop-categories-what-software-can-and-cannot-be).

The behaviour is specified in
[configuration-schema.md](../configuration-schema.md#safety) under
`safety.stop_chain`, and its HTTP surface in
[http-api.md](../http-api.md#safe-state-software-stop).

## The problem

When a machine's emergency stop removes power from devices, every device behind
the cut goes silent at once while the providers, which run on the controller,
stay up. Unless `safety.stop_chain` lists those devices and its `status` signal
already reads `in_force` when they go silent, the runtime reads that as a fleet
of device faults. It cannot tell an intended stop from a fault that looks the
same: a dead supply, an unplugged backplane, a blown fuse.

The runtime already does the safety-relevant part, whatever the cause. A lost
device is latched, the behaviour tree may not actuate it until an operator
re-enters AUTO, and the active safe-state rung is re-run for it when it
returns. The RFC adds:

- classification: reporting "emergency stop in force" instead of a fault, and
  not churning through fault handling for a stop;
- a reset that covers manual calls: the device-loss latch holds only the
  behaviour tree, so after power returns an operator's manual call reaches the
  actuator with no reset step in between;
- surviving a restart: persisting the software stop's latch, so a redeploy
  keeps it.

## Terms

A stop is described on two separate axes. Mixing them is what made the machine
profile's `estop_topology` values (`power_cut`, `signal`) ambiguous; see
[safety.md](../safety.md#the-machine-profile-already-names-this).

| Axis | Values |
| --- | --- |
| Scope: what loses power | actuator power only (logic, sensors and the bus stay up), or the whole device |
| Means: how | a contactor or relay on a supply rail; a rated safe-torque-off input (STO, IEC 61800-5-2); a device's stop input read by its firmware |

Scope "whole device" is valid, and common on small rigs whose devices cannot
separate logic from actuator power. It costs the controller its view of the
devices.

A stop input read by ordinary firmware is a monitoring and coordination signal,
not a safety path. Unless the device is safety-rated, the safety function is
the contactor or a rated STO. Either way, the runtime **only observes**. It is
never in the safety path, and no stop's effect may depend on it.

## What was decided

Each increment shipped on its own. Without `safety.stop_chain` in the config,
only increments 1 and 2 change behaviour.

1. AUTO is refused, with the reason, while the software stop's latch is
   engaged (#488). FAULT exits only to MANUAL, and MANUAL -> AUTO stays
   refused until the latch is cleared.
2. The latch is persisted under `data_dir`, keyed by the config file's
   directory (#489). The workbench starts every project with the same working
   directory and the same `runtime.name`, so a file keyed by either would let
   one project's latch hold another. A project's variants share their config
   directory, so they share the latch. A corrupt or unreadable file, or one
   that names another config directory, starts the runtime latched. A default
   or relative `data_dir` resolves against the config file's directory (#631).
3. `stop_chain.status` names a bool signal any provider publishes, as
   `provider/device/signal` (#490). It reads `in_force`, `clear` or `unknown`,
   and a reading that is stale, missing, not a bool or not `QUALITY_OK` is
   `unknown`, never `clear`. Prefer a contactor's mirror contact as the
   source, since it shows that power was removed and also shows a welded main
   contact, and wire it so that a broken wire reads as stopped. On `in_force`
   or `unknown` the latch engages without running its ladder, so no calls go
   to devices that may be unpowered. After startup, an automation machine with
   HTTP enabled also enters FAULT; at startup the mode stays IDLE and AUTO is
   refused while the latch holds. The clear is refused with `409` until the
   status reads `clear`, so `SafeStateController::clear()` returns a reason
   instead of `void`.
4. `stop_chain.devices` takes an optional `expect` per device, `power_loss` or
   `reports_estop` (#503). A device that contradicts it while the stop is in
   force raises "Stop not reaching *device*". The hold is the same either way.
5. The workbench shows the stop status and the devices in scope, and the
   existing two-step clear is the reset (anolishq/anolis-workbench#641,
   anolishq/anolis-workbench#644).

The latch refuses every function not declared READ, CONFIG included, manual
calls included. Safe-state calls are exempt, and so is the FAULT-entry hook a
software stop with no declared safe state lets through (#251).

The runtime never:

- drives a stop input or the stop chain. A device's stop input asserted by
  software is an ordinary function in the software-stop ladder
  (`safety.safe_state`), and is not evidence about the stop chain;
- infers a stop from a declaration, or from simultaneous device loss;
- clears a hold because the stop was released. ISO 13850:2015 §4.1.4:
  disengagement "shall not restart the machinery but only permit restarting".

## Proposed: rename `/v0/estop` (increment 6)

`POST /v0/estop` and `POST /v0/estop/clear` are a software stop, not an
emergency stop. Give them a software-stop name in the OpenAPI file and in the
runtime's messages, keep `/v0/estop` and `/v0/estop/clear` as deprecated
aliases that answer identically, and move the workbench to the new paths
(#485).

## Open question

Mode-transition hooks bypass the device-loss latch on purpose: refusing them
would veto the MANUAL -> AUTO transition that re-arms it. Should hooks on other
transitions be refused, or warned about, when they target a latched device?
