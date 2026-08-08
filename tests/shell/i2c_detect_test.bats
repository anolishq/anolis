#!/usr/bin/env bats
# Tests for GPIO-bus detection (anolishq/anolis#249).
#
# The original check globbed /dev/i2c-* and so matched the HDMI DDC adapters
# (i2c-20 / i2c-21) that any Pi with a display publishes regardless of
# `dtparam=i2c_arm=on`. It therefore reported "i2c: already enabled" on a machine
# with no GPIO bus, skipped the enablement, and the install failed 30s later with
# the providers unable to open /dev/i2c-1 and nothing naming the cause.
#
# It shipped undetected because nothing could exercise it without real hardware;
# _arm_i2c_bus_present takes overridable roots so these run anywhere.

INSTALL_SH="${BATS_TEST_DIRNAME}/../../tools/install.sh"

setup() {
    export ANOLIS_INSTALL_SH_NO_MAIN=1
    source "${INSTALL_SH}"
    set +u

    FAKE="$(mktemp -d)"
    mkdir -p "${FAKE}/dev" "${FAKE}/sys"
    export ANOLIS_I2C_DEV_ROOT="${FAKE}/dev"
    export ANOLIS_I2C_SYS_ROOT="${FAKE}/sys"
}

teardown() {
    [[ -n "${FAKE:-}" ]] && rm -rf "${FAKE}"
}

# Publish an adapter as the kernel does: /sys/class/i2c-dev/<n>/name + /dev/<n>.
_publish() {
    local node="$1" adapter_name="$2"
    mkdir -p "${ANOLIS_I2C_SYS_ROOT}/${node}"
    echo "${adapter_name}" > "${ANOLIS_I2C_SYS_ROOT}/${node}/name"
    touch "${ANOLIS_I2C_DEV_ROOT}/${node}"
}

@test "#249: HDMI DDC buses alone do NOT count as an enabled GPIO bus" {
    # Exactly what a desktop Pi with no dtparam looks like — the regression.
    _publish i2c-20 "fef04500.i2c"
    _publish i2c-21 "fef09500.i2c"

    run _arm_i2c_bus_present
    [ "$status" -ne 0 ]
}

@test "#249: the GPIO bus node counts" {
    _publish i2c-20 "fef04500.i2c"
    _publish i2c-1 "bcm2835 (i2c@7e804000)"

    run _arm_i2c_bus_present
    [ "$status" -eq 0 ]
}

@test "#249: a renumbered ARM bus is found by adapter name" {
    # No /dev/i2c-1, but the BSC controller is present under another number.
    _publish i2c-20 "fef04500.i2c"
    _publish i2c-11 "bcm2835 (i2c@7e804000)"
    rm -f "${ANOLIS_I2C_DEV_ROOT}/i2c-1"

    run _arm_i2c_bus_present
    [ "$status" -eq 0 ]
}

@test "#249: no adapters at all is not enabled" {
    run _arm_i2c_bus_present
    [ "$status" -ne 0 ]
}
