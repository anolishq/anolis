# RFC: observing the emergency stop (stop chain)

**Status:** proposed, not implemented.
**Depends on:** the stop categories in [safety.md](../safety.md). Tracked as
anolishq/anolis#284, part of anolishq/anolis#283.

## The problem

When a machine's emergency stop removes power from devices, every device behind
the cut goes silent at once while the providers, which run on the controller,
stay up. The runtime reads that as a fleet of device faults. It cannot tell an
intended stop from a fault that looks the same: a dead supply, an unplugged
backplane, a blown fuse.

The runtime already does the safety-relevant part, whatever the cause. A lost
device is latched, the behaviour tree may not actuate it until an operator
re-enters AUTO, and the active safe-state rung is re-run for it when it returns. What's
missing is:

- **classification:** saying "emergency stop in force" instead of "fault", and
  not churning through fault handling for a stop;
- **a reset that covers manual calls:** the device-loss latch holds only the
  behaviour tree, so after power returns an operator's manual call reaches the
  actuator with no reset step in between;
- **surviving a restart:** the software stop's latch is in memory only, so a
  redeploy drops it.

## Terms

The stop is described on two separate axes. Mixing them is what made the old
`estop_topology` values (`power_cut`, `signal`) ambiguous.

| Axis | Values |
| --- | --- |
| **Scope**: what loses power | actuator power only (logic, sensors and the bus stay up), or the whole device |
| **Means**: how | a contactor or relay on a supply rail; a rated safe-torque-off input (STO, IEC 61800-5-2); a device's stop input read by its firmware |

Scope "actuator power only" is the usual industrial arrangement. Scope "whole
device" is valid, and common on small rigs where devices can't separate the two.
It costs the controller its view of the devices.

A stop input read by ordinary firmware is a **monitoring and coordination
signal**, not a safety path. Unless the device is safety-rated, the safety
function is the contactor or a rated STO. Either way, the runtime only
**observes**. It is never in the safety path, and no stop's effect may depend
on it.

## The model

```yaml
# runtime config. Absent means today's behaviour, unchanged.
safety:
  stop_chain:
    status:                         # optional
      signal: "gpio0/estop/chain_open"
      active: true                  # the signal's value meaning "stop in force"
    devices:                        # the scope
      - bread0/rlht0                # held while in force; no expectation
      - handle: bread0/dcmt0
        expect: power_loss          # goes silent when stopped
      - handle: bread0/dcmt1
        expect: reports_estop       # stays on the bus, reports estop=true
```

- **`status`** names a boolean signal that any provider publishes: a GPIO
  provider, a PLC bridge, a device's input, or a safety relay's status output.
  The runtime refers only to `provider/device/signal`, so nothing in core names
  hardware.
  - Prefer a contactor's mirror contact, the mechanically linked auxiliary
    contact: it reports that power was actually removed, and also reveals a
    welded main contact.
  - Wire it so that a broken wire reads as stopped, never as clear.
- **`devices`** lists what the stop affects. A device the stop doesn't affect is
  left out.
- **`expect`** is optional per device:
  - `power_loss`: the device goes silent;
  - `reports_estop`: it stays on the bus and reports its `estop` signal true;
  - omitted: the device is held, and no mismatch is raised. This covers a device
    whose actuator supply is cut while its logic keeps running and it has no stop
    input.

On load, the runtime warns when:

- the status source is itself a device in `devices`. It would go silent with the
  stop, so it can't separate a stop from a power fault.
- the status source's device also has actuating functions. The protocol doesn't
  link a signal to the function that drives it, so this is a heuristic: a status
  line that the runtime can also drive would read back a software action as a
  stop.

### States and classification

`status` reads `in_force`, `clear` or `unknown`. A source that is stale,
unreadable or unregistered is `unknown`, never `clear`.

| Status | Devices in scope | Reported | Done |
| --- | --- | --- | --- |
| in_force | gone, or reporting as expected | "Emergency stop in force" | hold; no device fault churn |
| in_force | a declared `expect` contradicted | "Stop not reaching *device*" | hold, plus an alarm |
| clear | lost | device fault, as today | today's fault handling |
| unknown | any | "Stop status unavailable (emergency stop or power fault)" | hold |
| not configured | lost | as today | as today. A stop is never inferred from simultaneous loss. At most, the runtime may report "correlated loss, cause unknown" |

### Hold and reset

ISO 13850: releasing an emergency stop "shall not restart the machinery but
only permit restarting".

- When the status goes `in_force` or `unknown`, the runtime **engages the
  software stop's latch without running its ladder**. On an automation machine
  it also enters FAULT. It sends no device calls: the devices in scope may be
  unpowered, and commanding them would only produce failed actions, which is the
  churn this RFC removes. The hardware stop is what makes them safe.
- The latch refuses every actuating call, manual ones included. "Actuating" fails
  closed: every function not marked READ, CONFIG included. Two calls are exempt:
  - safe-state actions;
  - the FAULT-entry hook that a software stop with no declared safe state lets
    through for its own FAULT entry (#251).
- AUTO entry is refused, with the reason, while the latch is engaged or the
  status is `in_force` or `unknown`. Today a software stop also enters FAULT, and
  FAULT can only exit to MANUAL. But MANUAL -> AUTO is not refused while the
  latch is still engaged, nor after a FAULT entry that failed. In those cases the
  behaviour tree's calls are refused one by one.
- Clearing the latch is refused while the status is `in_force` or `unknown`.
  `SafeStateController::clear()` returns a reason instead of `void`, so every
  caller gets the check. `POST /v0/estop/clear` answers it as FAILED_PRECONDITION
  rather than its current unconditional 200 `{latched: false}`.
- Once the stop is released, one deliberate clear by the operator permits a
  restart. Releasing the stop never clears anything.

### Surviving a restart

- The latch is persisted to a file under `data_dir`, keyed per runtime config,
  for example `<data_dir>/estop/<config-name>.json`. It is written with
  write-temp, fsync and rename when the latch engages, and removed when it
  clears. If the file is present at startup, the runtime starts latched. A
  corrupt or unreadable file also means latched.
  - The key matters because the workbench starts every project it launches with
    the same working directory. With one shared file, project A's latch would
    hold project B.
  - Installs already keep `data_dir` under the install prefix, through the
    systemd unit's working directory. It must stay on persistent storage.
- At startup the runtime also checks the stop status, after provider discovery
  and the first poll, with a bounded wait. If it reads `in_force` or `unknown`
  then, the runtime starts latched. That covers a stop pressed while the runtime
  was down. It also means a machine whose status source isn't up at boot starts
  latched. That is intended: it fails toward held.

### Things the runtime never does

- Drive a stop input or the stop chain. If software asserts a device's stop
  input, that is an ordinary device function in the software-stop ladder
  (`safety.safe_state`), and a stop input the runtime asserted is not evidence
  about the stop chain.
- Infer a stop from a declaration, or from simultaneous loss.
- Release a hold because the stop was released.

## Open decisions

1. **Mode-transition hooks and the device-loss latch.** Hooks bypass the
   device-loss latch on purpose: refusing them would veto the MANUAL -> AUTO
   transition that re-arms it. Should hooks on other transitions be refused, or
   warned about, when they target a latched device?
2. **The `/v0/estop` path.** It is a software stop. Rename it as the last
   increment, keeping `/v0/estop` as a deprecated alias.

## Increments

Each ships on its own. Without a `stop_chain` in config, none of them changes
behaviour except steps 1 and 2, which apply to every machine.

1. Refuse AUTO, with a reason, while the software stop's latch is engaged.
2. Persist the latch, keyed per runtime config.
3. `stop_chain.status`: the states, the hold, and refusing the clear. This
   changes `SafeStateController::clear()`, and the HTTP contract of
   `POST /v0/estop/clear`, which gains a FAILED_PRECONDITION reply, in the
   OpenAPI file. The workbench's clear handling has to change with it: its
   epoch logic (anolishq/anolis-workbench#617) reads `latched` from the reply.
4. `devices` and `expect`, with the mismatch alarm. The runtime then reads
   devices' `estop` signals, which nothing does today.
5. Workbench: show the stop status and a guided reset (anolishq/anolis-workbench#572).
6. The `/v0/estop` rename.

## Example: one rig, two arrangements

The reference rig has I2C modules on a shared 12 V backplane. Each module's
microcontroller has a stop input.

- **gen1 backplane:** one rail feeds both logic and actuators, so the scope is
  the whole device. A mirror contact on the e-stop contactor goes to an input on
  the controller side of the cut, read by a GPIO provider. Its devices are
  `expect: power_loss`. Wiring the backplane's stop line as well adds nothing,
  since the modules lose power anyway.
- **gen2 backplane:** a normally-closed stop chain opens the motor and relay
  power paths while the modules keep logic power. Its devices are
  `expect: reports_estop` if the same chain drives their stop inputs, or no
  expectation if only the power path opens.

Both are configuration. Neither is a code path.
