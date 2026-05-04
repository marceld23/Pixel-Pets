# Security Policy

Pixel Pets is a hobby firmware project for M5Stack hardware. It runs locally, has no cloud back-end, no user accounts, no payment paths, and no exposed network services beyond a captive-portal Wi-Fi setup and an optional `<pet>-setup.local` parent dashboard on the user's home network.

The threat model is: *don't make a kid's toy do something obviously bad on the user's local network.*

## Supported versions

Only the latest release on `main` is supported. We don't backport.

## Reporting a vulnerability

For any sensitive issue, please use the repo's [**Security tab → Report a vulnerability**](https://github.com/marceld23/Pixel-Pets/security/advisories/new). That sends a private report straight to the maintainers without going public first.

For less sensitive issues — for example, a denial-of-service against the pet itself with no real-world impact beyond annoying the kid — a regular GitHub issue is fine.

## Response time

We're a two-person family project, not an on-call rotation. Best-effort response is usually a few days, occasionally a couple of weeks if school holidays land badly. We'll acknowledge receipt and keep you in the loop.

## Out of scope

- Issues that require physical access to the device (this is firmware on hardware you own).
- Issues exposed only via Wi-Fi credentials the user explicitly entered.
- Functional bugs that aren't security issues — those go in the regular issue tracker.
