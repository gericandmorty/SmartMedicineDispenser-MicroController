# Scheduled buzzer and compartment lights

Upload the updated sketches to all four controllers: `arduino`, `esp1`, `esp2`,
and `esp32-mini-code`. Arduino-only flashing will not update the drawer lights.

- A scheduled Arduino alarm beeps 1 second on / 1 second off for up to **20 minutes**.
  Opening the alerting drawer silences it early. The startup beep remains 10 seconds,
  and the manual buzzer test remains 2 seconds.
- Saving a schedule does not illuminate its compartment. With the drawer open,
  only the current due compartment blinks. Closed drawers keep their lights off.
- The latest due occurrence replaces the previous indication, including across
  drawers. For example, at 11:45 only the 11:45 compartment is indicated; at 11:46
  only the 11:46 compartment is indicated; at 11:47 only the 11:47 compartment is
  indicated. Replacing an indication does not mark the older dose as taken.
- Pickup is confirmed by opening and then closing the drawer, or by an explicit
  confirmation request. Its light clears and stays off if the drawer is reopened.
  The light also expires after the 20-minute alert window.
- `date=YYYY-MM-DD` schedules fire once on that local date. An omitted/empty `date`
  repeats daily, once per day at its time. Time matching uses GMT+8.
- Re-saving an unchanged schedule with the same ID preserves its fired state.
  The ESP32 master persists that state in flash. Arduino direct schedules and
  their fired state remain in RAM, as before, and must be restored after reboot.

Use a single compartment per alarm request. If multiple schedules share the same
minute, the last matching schedule becomes the current indication.

## Verification

Run `python3 microcontroller/tests/run_behavior_tests.py` from the project root.
The tests compile firmware logic using `g++` with simulated time, GPIO, and
network/storage boundaries. They cover consecutive alarm times, cross-drawer
selection, pickup, retries, daily recurrence, date restrictions, master flash
restoration, and buzzer/LED expiry. They do not replace board compilation or a
physical check of the reed switches, relay, buzzer, and LEDs.

For a bench check, schedule three different compartments one minute apart. Open
and close the correct drawer at each due time, then reopen it to verify that the
collected occurrence stays dark. Also leave an alarm uncollected to check that
the next due time replaces its light and the final alarm stops after 20 minutes.
