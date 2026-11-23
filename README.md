# Scheduled buzzer and compartment lights

Upload the updated sketches to all four controllers: `arduino`, `esp1`, `esp2`,
and `esp32-mini-code`. Arduino-only flashing will not update the drawer lights.

- A scheduled Arduino alarm beeps 1 second on / 1 second off for up to **5 minutes**.
  Opening the alerting drawer silences it early. The pickup window and LCD countdown also end after 5 minutes. The startup beep remains 10 seconds,
  and the manual buzzer test remains 2 seconds.
- Opening a drawer to retrieve a due dose, or confirming a dose while a drawer
  remains open, starts the cabinet-close warning. The LCD and app keep the
  door prompt visible until **both** reed switches confirm stable closure.
- Opening either drawer outside an intentional refill pulses the relay for up to 30 seconds. The timer
  resets once both drawers close. A dose that expires while a drawer stays
  open is still recorded as missed and does not restart the sound.
- With both drawers closed, a missed dose pulses the relay for 30 seconds
  (250 ms on / 250 ms off), including when a newer dose supersedes it.
- Saving a schedule does not illuminate its compartment. With the drawer open,
  only the current due compartment blinks. Closed drawers keep their lights off.
- The latest due occurrence replaces the previous indication, including across
  drawers. For example, at 11:45 only the 11:45 compartment is indicated; at 11:46
  only the 11:46 compartment is indicated; at 11:47 only the 11:47 compartment is
  indicated. An uncollected older dose is recorded as missed when the next dose starts.
- Pickup is confirmed by opening and then closing the drawer, or by an explicit
  confirmation request. Its light clears and stays off if the drawer is reopened.
  The light also expires after the 5-minute alert window.
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
the next due time records it as missed and starts the new alarm. With no later
alarm, the active dose expires after 5 minutes.

## Missed medicine and completion

A missed dose sets `unresolved_missed_dose` on Arduino `/status`. Acknowledging it
in the app releases the latch for subsequent due-dose alarms. Each miss gets its
own 30-second warning. When every currently due dose is collected and there is
no outstanding missed medicine, the relay pulses 200 ms on / 200 ms off for
30 seconds or until Add Medicine starts a refill. A partial pickup has no
confirmation chirp. The Arduino sends an explicit pickup report to the ESP32 over UART
and Wi-Fi. The separate vibration mute command leaves dose history intact.
